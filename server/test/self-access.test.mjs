// The server access settings of a Docker Compose install without an updater
// (src/self-access.mjs): the policy in the data volume, a save applied by a
// restart of the broker itself, the check of the new addresses and the way
// back, and following the public IP. Real files in a temporary data folder and
// the real configuration loader; a "restart" is a new load of that folder.
// No Docker, no listener and no network: the reach of an address and the
// public IP lookup are answers given here (the tests named "the real server"
// listen on loopback and ask that listener).
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readFile, writeFile, readdir, unlink } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { randomBytes, randomUUID } from 'node:crypto';
import { createServer, request as httpRequest } from 'node:http';
import { createServer as createTlsServer } from 'node:https';
import { spawnSync } from 'node:child_process';
import { configuration, revertUnloadableSelfAccess, readProtectedAccessFile } from '../src/config.mjs';
import { SelfAccess, selfAccessRestartCode, publicIpv4, lookupPublicIp, defaultGateway, probeAddress, accessProbeProtocol } from '../src/self-access.mjs';
import { buildServer, VERSION } from '../src/http.mjs';
import { main as accessCli } from '../src/access-cli.mjs';
import { selfAccessStorage } from '../src/config.mjs';
import { localAccessRequest, validAccessHealth } from '../update/server-access-health.mjs';
import { validBootstrapLocalHealth, bootstrapHealthOrigin } from '../update/deployment-health.mjs';
import { serverAccessTransport } from '../update/server-access-policy.mjs';
import { AccessRuntime } from '../src/access-runtime.mjs';
import { resolveRequestAccess } from '../src/request-origin.mjs';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { serverAccessHash, serverAccessUnconfiguredRevision } from '../update/server-access-policy.mjs';

const lan = 'http://192.0.2.10:18480', extra = 'http://vdi.example.com:18480', secure = 'https://vdi.example.com';
// The data folder's own files, without the owner and mode rules a real volume adds (server-access-storage-reader.test.mjs has those).
const readAccessFile = path => { try { return readFileSync(path, 'utf8'); } catch (error) { if (error.code === 'ENOENT') return null; throw error; } };
const quiet = () => ({ protocol: 'server-access-activity-v1', fence: true, connections: { people: 0, sessions: 0, consoles: 0 },
  activity: { protocol: 'activity-v1', sessions: { total: 0, stream: 0, attached: 0, tickets: 0 },
    channels: { clipboard: 0, clipboardTransfers: 0, drive: 0, driveShares: 0, driveRequests: 0, usb: 0, usbDevices: 0, print: 0, printTransfers: 0 },
    operations: { display: 0, sessionCleanup: 0, hostUpdates: 0, power: 0 }, hosts: { connected: 0 }, live: { channels: 0 }, sinceActivityMs: 1000, uptimeMs: 5000 } });
const policy = ({ http = [lan], https = [], primaryOrigin = https[0] ?? http[0], follow, trustedProxies = ['127.0.0.1', '::1'], listenPort = 18480 } = {}) => ({ protocol: 'server-access-v1', primaryOrigin,
  https: { enabled: https.length > 0, origin: https[0] ?? null, ...(https.length > 1 ? { extraOrigins: https.slice(1) } : {}), mode: 'proxy', listenHost: '0.0.0.0', listenPort },
  http: { enabled: http.length > 0, origin: http[0] ?? null, ...(http.length > 1 ? { extraOrigins: http.slice(1) } : {}), listenHost: '0.0.0.0', listenPort, ...(follow === undefined ? {} : { followPublicIp: follow }) }, trustedProxies });
const refused = (status, code) => error => error.status === status && error.code === code;

async function volume(t) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-self-access-'));
  t.after(() => rm(directory, { recursive: true, force: true }));
  return directory;
}
// One process of the broker on that data folder: its configuration and the
// access client index.mjs makes from it. `exits` counts the restarts it asks for.
function start(directory, { env = {}, healthy = true, reach = true, lookup = async () => null, activity = quiet, ...options } = {}) {
  const settings = { MULTITAP_HOST: '0.0.0.0', MULTITAP_PORT: '18480', MULTITAP_DEPLOYMENT_KIND: 'docker', MULTITAP_DATA_DIR: directory, MULTITAP_PUBLISHED_PORT: '0.0.0.0:18480', MULTITAP_SERVER_ADDRESS: '192.0.2.10',
    MULTITAP_HTTP_ORIGIN: lan, MULTITAP_ALLOW_INSECURE_HTTP: '1', ...env };
  const config = configuration(settings, { readAccessFile });
  const run = { settings, config, exits: 0, probes: [], tried: [], lookups: 0, ended: 0, logs: [], held: [], released: [], records: [] };
  // What 연결 테스트 finds at a main address: this server's own answer, or what `reach` says instead.
  const probeRequest = async (origin, nonce) => { run.tried.push(origin);
    const found = typeof reach === 'function' ? reach(origin, nonce) : reach;
    return found === true ? { answer: { protocol: accessProbeProtocol, nonce, admitted: true } } : found; };
  // What the address answers after the restart: this installation's own health, or nothing.
  const publicRequest = async (current, origin) => { run.probes.push(origin);
    return (typeof healthy === 'function' ? healthy(origin) : healthy) ? { service: 'multitap-broker', status: 'ok', authentication: 'id-password', version: '1.1.1',
      access: { protocol: 'server-access-v1', revision: serverAccessHash(current), installationId: config.accessInstallationId, bootstrapOnly: false, origin } } : null; };
  run.access = SelfAccess.from(config, { exit: () => { run.exits++; }, load: () => configuration(settings, { readAccessFile }), publicRequest, probeRequest, lookup: async () => { run.lookups++; return lookup(); }, log: line => run.logs.push(line), sleep: async () => {}, pollMs: 1, verifyAttempts: 2, verifyDelayMs: 1, ...options });
  run.access.attach({ activity, endConnections: () => { run.ended++; return { protocol: 'server-access-connections-v1', fence: true, ended: { people: 0, sessions: 0, consoles: 0 } }; },
    hold: lease => { run.held.push(lease); return lease; }, release: lease => run.released.push(lease), record: (...row) => run.records.push(row) });
  run.context = { managementOrigin: config.accessConfig?.primaryOrigin ?? lan };
  // The console's 연결 테스트 and 변경 저장, as AccessRuntime passes them on.
  run.change = async config => {
    const base = run.access.status().revision, tested = await run.access.test({ config, baseRevision: base }, run.context);
    const saved = await run.access.save({ config, testId: tested.testId, baseRevision: base, confirm: true }, { ...run.context, fence: { token: randomBytes(32).toString('hex'), operationId: randomUUID() } });
    await run.access.task;
    return saved.operationId;
  };
  return run;
}
const journal = async directory => JSON.parse(await readFile(join(directory, 'access', 'journal.json'), 'utf8'));

test('a save writes the policy into the volume, asks for a restart, and the restarted server keeps it once every main address answers', async t => {
  const directory = await volume(t), first = start(directory);
  await first.access.initialize();
  assert.equal(first.config.selfAccess.saved, false, 'the .env lines are the policy until one is saved');
  const before = first.access.status();
  assert.equal(before.selfManaged, true); assert.equal(before.applySupported, true); assert.equal(before.managed, true); assert.equal(before.deploymentKind, 'docker'); assert.equal(before.legacy, true);
  assert.equal(before.revision, first.config.accessRevision);
  assert.deepEqual(before.fixed, { listenHost: '0.0.0.0', listenPort: 18480, httpsMode: 'proxy', certFile: null, keyFile: null, idlePorts: { https: 18443, http: 18481 }, publishedPort: 18480, publishedHost: '0.0.0.0', serverAddress: '192.0.2.10' });
  assert.deepEqual(before.dockerPorts, ['https', 'http'].map(scheme => ({ hostPort: 18480, containerPort: 18480, protocol: 'tcp', scheme })));

  // The page's listen fields are not the page's to choose: the container's are kept.
  const operationId = await first.change(policy({ http: [lan, extra], listenPort: 9999 }));
  assert.equal(first.exits, 1, 'one restart asked for');
  assert.equal(selfAccessRestartCode, 75);
  assert.equal(first.access.status().state, 'applying'); assert.deepEqual(first.access.status().applied ?? first.access.status().config, before.config);
  const saved = JSON.parse(await readFile(join(directory, 'access', 'policy.json'), 'utf8'));
  assert.deepEqual(Object.keys(saved).sort(), ['config', 'envSeed', 'protocol', 'revision', 'schema', 'updatedAt']);
  assert.equal(saved.envSeed, first.config.selfAccess.envSeed, 'the .env address lines it was saved beside');
  assert.equal(saved.config.http.listenPort, 18480); assert.deepEqual(saved.config.http.extraOrigins, [extra]);
  assert.match(await readFile(join(directory, 'access', 'identity'), 'utf8'), /^[a-f0-9]{64}\n$/);
  // Nothing under the names a 1.1.1 server reads in the data folder and
  // refuses unless root owns them (server-access.json, server-access.identity):
  // that server still starts on this volume, with its .env addresses.
  assert.deepEqual(await readdir(directory), ['access']);
  for (const name of ['server-access.json', 'server-access.identity']) assert.equal(readProtectedAccessFile(join(directory, name)), null, name);
  assert.deepEqual((await readdir(join(directory, 'access'))).sort(), ['identity', 'journal.json', 'policy.json']);

  const second = start(directory);
  assert.equal(second.config.selfAccess.saved, true);
  assert.deepEqual(second.config.allowedOrigins, [lan, extra]);
  await second.access.initialize();
  assert.equal(second.access.startupOperationId, operationId, 'the new process starts fenced for this save');
  // The fence of the restarted server, as http.mjs makes it: closed like a restart until the result is read.
  const runtime = new AccessRuntime({ client: second.access, config: second.config.accessConfig, revision: second.config.accessRevision, installationId: second.config.accessInstallationId, initialOperationId: second.access.startupOperationId });
  t.after(() => runtime.close());
  assert.throws(() => runtime.assertAdmission(), error => error.status === 503 && error.code === 'server_restarting');
  assert.equal((await runtime.status({ managementOrigin: lan })).pending.state, 'health_check'); assert.equal(runtime.blocked, true);
  const checking = second.access.status();
  assert.equal(checking.state, 'health_check'); assert.equal(checking.revision, before.revision); assert.deepEqual(checking.config, before.config);
  assert.deepEqual(checking.pendingConfig.http.extraOrigins, [extra]);
  await assert.rejects(second.access.test({ config: policy(), baseRevision: checking.revision }, second.context), refused(409, 'server_access_busy'));
  await second.access.resume();
  assert.deepEqual(second.probes, [lan], 'the main address only; an extra one may be a router forward the server cannot reach');
  const applied = second.access.status();
  assert.equal(applied.state, 'succeeded'); assert.equal(applied.operationId, operationId); assert.equal(applied.pendingConfig, null); assert.equal(applied.legacy, false);
  assert.equal(applied.revision, second.config.accessRevision); assert.deepEqual(applied.config.http.extraOrigins, [extra]);
  assert.equal(second.access.startupOperationId, null); assert.equal(second.exits, 0);
  const read = await runtime.status({ managementOrigin: lan });
  assert.equal(read.state, 'applied'); assert.equal(read.supported, true); assert.equal(read.pending, null); assert.equal(runtime.blocked, false, 'open again');
  // A later start reads the same result and has nothing to check.
  const third = start(directory); await third.access.initialize();
  assert.equal(third.access.status().state, 'succeeded'); assert.equal(third.access.startupOperationId, null);
  await third.access.resume(); assert.deepEqual(third.probes, []);
});

test('an address that does not answer as this installation puts the previous policy back, restarts again and says so', async t => {
  const directory = await volume(t), first = start(directory);
  await first.access.initialize();
  const revision = first.access.status().revision;
  await first.change(policy({ http: ['http://192.0.2.99:18480'] }));
  // The restart with the new policy: nothing answers there.
  const second = start(directory, { healthy: false });
  assert.deepEqual(second.config.allowedOrigins, ['http://192.0.2.99:18480']);
  await second.access.initialize(); await second.access.resume();
  assert.equal(second.probes.length, 2, 'asked again before giving up');
  assert.equal(second.exits, 1, 'the restart that loads the previous policy');
  assert.equal(second.access.status().state, 'health_check', 'not a result yet');
  assert.equal((await journal(directory)).state, 'rolling_back');
  assert.deepEqual(await readdir(join(directory, 'access')).then(list => list.sort()), ['identity', 'journal.json'], 'there was no saved policy before: there is none again');
  const third = start(directory);
  assert.deepEqual(third.config.allowedOrigins, [lan]); assert.equal(third.config.selfAccess.saved, false);
  await third.access.initialize();
  const result = third.access.status();
  assert.equal(result.state, 'rolled_back'); assert.equal(result.rollbackVerified, true); assert.equal(result.errorCode, 'server_access_health_failed');
  assert.equal(result.failedOrigin, 'http://192.0.2.99:18480', 'the address that did not answer is named');
  assert.deepEqual(third.records, [['server_access_rolled_back', 'server', result.operationId + ':http://192.0.2.99:18480']], 'and the way back is in the activity log');
  // index.mjs reads the journal before the server exists: what a start found is recorded once the server can write.
  const early = SelfAccess.from(third.config, { exit: () => {}, load: () => third.config }), written = [];
  early.record('server_access_rolled_back', 'server', 'found at the start');
  early.attach({ record: (...row) => written.push(row) });
  assert.deepEqual(written, [['server_access_rolled_back', 'server', 'found at the start']]);
  assert.equal(result.revision, revision); assert.equal(result.pendingConfig, null); assert.equal(third.access.startupOperationId, null);
  // A saved policy goes back to its exact earlier content.
  await third.change(policy({ http: [lan, extra] }));
  const fourth = start(directory); await fourth.access.initialize(); await fourth.access.resume();
  const kept = await readFile(join(directory, 'access', 'policy.json'), 'utf8');
  await fourth.change(policy({ http: [lan], https: [secure], primaryOrigin: secure }));
  const fifth = start(directory, { healthy: origin => origin !== secure });
  await fifth.access.initialize(); await fifth.access.resume();
  assert.deepEqual(fifth.probes.slice(0, 1), [secure]); assert.equal(fifth.exits, 1);
  assert.equal(await readFile(join(directory, 'access', 'policy.json'), 'utf8'), kept);
  const sixth = start(directory); await sixth.access.initialize();
  assert.equal(sixth.access.status().state, 'rolled_back'); assert.deepEqual(sixth.config.allowedOrigins, [lan, extra]);
  assert.equal(sixth.access.status().failedOrigin, secure, 'of two main addresses, the one that did not answer');
});

test('a process that ends between the journal and the policy changed nothing; a policy that does not load, or a process that keeps ending, goes back', async t => {
  const directory = await volume(t), first = start(directory);
  await first.access.initialize();
  await first.change(policy({ http: [lan, extra] }));
  // Ended before the policy was written.
  await unlink(join(directory, 'access', 'policy.json'));
  const second = start(directory); await second.access.initialize();
  const interrupted = second.access.status();
  assert.equal(interrupted.state, 'failed'); assert.equal(interrupted.errorCode, 'server_access_interrupted'); assert.equal(interrupted.unchangedVerified, true);
  assert.equal(second.access.startupOperationId, null); assert.equal(second.exits, 0);

  // A policy file that does not load: the start itself puts the previous one back.
  await second.change(policy({ http: [lan, extra] }));
  await writeFile(join(directory, 'access', 'policy.json'), '{"schema":1');
  // (index.mjs: a policy that cannot be used and that a save of this server has just written.)
  assert.equal(start(directory).config.selfAccess.unreadable, 'server_access_state_invalid');
  assert.equal(await revertUnloadableSelfAccess(second.settings, { readAccessFile }), true);
  const third = start(directory); await third.access.initialize();
  assert.deepEqual(third.config.allowedOrigins, [lan]);
  assert.equal(third.access.status().state, 'rolled_back'); assert.equal(third.access.status().errorCode, 'server_access_start_failed');
  assert.equal(await revertUnloadableSelfAccess(third.settings, { readAccessFile }), false, 'only for a save that is being applied');
  // Not for an install with an updater, whose files are the updater's.
  assert.equal(await revertUnloadableSelfAccess({ ...third.settings, MULTITAP_UPDATE_SOCKET: '/run/multitap-update/control.sock' }, { readAccessFile }), false);

  // Started three times without reaching its check: the fourth start goes back.
  await third.change(policy({ http: [lan, extra] }));
  for (let index = 0; index < 3; index++) { const again = start(directory); await again.access.initialize(); assert.equal(again.access.status().state, 'health_check'); assert.equal(again.exits, 0); }
  const fourth = start(directory); await fourth.access.initialize();
  assert.equal(fourth.exits, 1); assert.equal((await journal(directory)).errorCode, 'server_access_start_failed');
  const fifth = start(directory); await fifth.access.initialize();
  assert.equal(fifth.access.status().state, 'rolled_back'); assert.deepEqual(fifth.config.allowedOrigins, [lan]);
});

test('a save waits for the server\'s own work, can be withdrawn meanwhile, and a restart during the wait changed nothing', async t => {
  const directory = await volume(t);
  let updates = 1;
  const busy = () => { const report = quiet(); report.activity.operations.hostUpdates = updates; return report; };
  const first = start(directory, { activity: busy, sleep: () => new Promise(resolve => setImmediate(resolve)) });
  await first.access.initialize();
  const base = first.access.status().revision, next = policy({ http: [lan, extra] });
  const tested = await first.access.test({ config: next, baseRevision: base }, first.context);
  assert.deepEqual(tested.checks.map(check => [check.name, check.status]), [['configuration', 'passed'], ['managed_installation', 'passed'], ['address', 'passed'], ['address', 'passed']], 'the main and the extra address');
  assert.equal(tested.scope, 'preflight'); assert.equal(tested.requiresRestart, true); assert.equal(tested.baseRevision, base);
  const fence = { token: randomBytes(32).toString('hex'), operationId: randomUUID() };
  const accepted = await first.access.save({ config: next, testId: tested.testId, baseRevision: base, confirm: true }, { ...first.context, fence });
  assert.deepEqual(accepted, { accepted: true, operationId: fence.operationId, state: 'waiting_idle' });
  await new Promise(resolve => setTimeout(resolve, 20));
  const waiting = first.access.status();
  assert.equal(waiting.state, 'waiting_idle'); assert.deepEqual(waiting.blockers, [{ code: 'server_access_host_updates_busy', count: 1 }]); assert.ok(waiting.waitUntil);
  assert.deepEqual(waiting.pendingConfig.http.extraOrigins, [extra]);
  await assert.rejects(first.access.save({ config: next, testId: tested.testId, baseRevision: base, confirm: true }, { ...first.context, fence: { ...fence, operationId: randomUUID() } }), refused(409, 'server_access_busy'));
  await assert.rejects(first.access.cancel({ operationId: randomUUID() }), refused(409, 'server_access_cancel_unavailable'));
  // A restart while it waits: nothing had been written.
  const restarted = start(directory); await restarted.access.initialize();
  assert.equal(restarted.access.status().state, 'failed'); assert.equal(restarted.access.status().errorCode, 'server_access_interrupted'); assert.equal(restarted.access.status().unchangedVerified, true);

  const cancelled = await first.access.cancel({ operationId: fence.operationId });
  assert.equal(cancelled.cancelled, true);
  const withdrawn = first.access.status();
  assert.equal(withdrawn.state, 'failed'); assert.equal(withdrawn.errorCode, 'server_access_cancelled'); assert.equal(withdrawn.unchangedVerified, true); assert.equal(withdrawn.pendingConfig, null);
  assert.equal(first.exits, 0); assert.equal(first.ended, 0, 'nobody was disconnected');
  assert.deepEqual((await readdir(join(directory, 'access'))).sort(), ['journal.json']);
  // The work ends: the next save goes through, ending the connections that are open.
  updates = 0;
  let open = 1;
  const second = start(directory, { activity: () => { const report = quiet(); report.activity.sessions = { total: open, stream: open, attached: open, tickets: 0 }; report.connections = { people: open, sessions: open, consoles: 0 }; return report; } });
  second.access.attach({ ...second.access.hooks, endConnections: () => { second.ended++; open = 0; return { protocol: 'server-access-connections-v1', fence: true, ended: { people: 1, sessions: 1, consoles: 0 } }; } });
  await second.access.initialize(); await second.change(next);
  assert.equal(second.ended, 1); assert.equal(second.exits, 1);
});

test('what a save must name: a current test of this exact change, its base revision, a console address in use and a new operation', async t => {
  const directory = await volume(t), run = start(directory);
  await run.access.initialize();
  const base = run.access.status().revision, next = policy({ http: [lan, extra] }), fence = () => ({ token: randomBytes(32).toString('hex'), operationId: randomUUID() });
  const tested = await run.access.test({ config: next, baseRevision: base }, run.context);
  const save = (input, context = { ...run.context, fence: fence() }) => run.access.save({ config: next, testId: tested.testId, baseRevision: base, confirm: true, ...input }, context);
  await assert.rejects(save({ confirm: false }), refused(400, 'server_access_confirmation_required'));
  await assert.rejects(save({ baseRevision: 'f'.repeat(64) }), refused(409, 'server_access_revision_changed'));
  await assert.rejects(save({ config: policy({ http: [lan, 'http://other.example.com:18480'] }) }), refused(409, 'server_access_test_required'));
  await assert.rejects(save({ testId: 'e'.repeat(64) }), refused(409, 'server_access_test_required'));
  await assert.rejects(save({}, { managementOrigin: 'http://192.0.2.77:18480', fence: fence() }), refused(409, 'server_access_management_origin_changed'));
  await assert.rejects(save({}, { ...run.context, fence: { token: 'short', operationId: randomUUID() } }), refused(400, 'server_access_fence_invalid'));
  await assert.rejects(run.access.test({ config: next, baseRevision: 'f'.repeat(64) }, run.context), refused(409, 'server_access_revision_changed'));
  await assert.rejects(run.access.test({ config: { ...next, primaryOrigin: 'http://elsewhere.example.com' }, baseRevision: base }, run.context), refused(400, 'server_access_primary_origin_invalid'));
  await assert.rejects(run.access.test({ config: next, baseRevision: base, extra: 1 }, run.context), refused(400, 'server_access_command_invalid'));
  // The policy in use is no change.
  const same = await run.access.test({ config: policy(), baseRevision: base }, run.context);
  await assert.rejects(run.access.save({ config: policy(), testId: same.testId, baseRevision: base, confirm: true }, { ...run.context, fence: fence() }), refused(400, 'server_access_change_required'));
  assert.equal(run.exits, 0); assert.equal(run.access.status().state, 'idle');
  await assert.rejects(readdir(join(directory, 'access')), { code: 'ENOENT' }, 'nothing was written');
});

test('the first setup of a fresh install saves its first address; an unreachable one leaves it a fresh install', async t => {
  const directory = await volume(t), fresh = { NODE_ENV: 'production', MULTITAP_HTTP_ORIGIN: '', MULTITAP_ALLOW_INSECURE_HTTP: '0' };
  const first = start(directory, { env: fresh });
  assert.equal(first.config.bootstrapOnly, true); assert.equal(first.config.accessConfig, null);
  await first.access.initialize();
  const empty = first.access.status();
  assert.equal(empty.config, null); assert.equal(empty.revision, serverAccessUnconfiguredRevision); assert.equal(empty.applySupported, true);
  await first.change(policy());
  const second = start(directory, { env: fresh, healthy: false });
  assert.equal(second.config.bootstrapOnly, false); assert.deepEqual(second.config.allowedOrigins, [lan]);
  await second.access.initialize(); await second.access.resume(); assert.equal(second.exits, 1);
  const third = start(directory, { env: fresh });
  assert.equal(third.config.bootstrapOnly, true, 'a fresh install again');
  await third.access.initialize();
  assert.equal(third.access.status().state, 'rolled_back'); assert.equal(third.access.status().config, null);
  await third.change(policy());
  const fourth = start(directory, { env: fresh }); await fourth.access.initialize(); await fourth.access.resume();
  assert.equal(fourth.access.status().state, 'succeeded'); assert.deepEqual(fourth.access.status().config.http.origin, lan);
});

test('what the container cannot serve is refused before anything is written', async t => {
  const directory = await volume(t), run = start(directory);
  await run.access.initialize();
  const base = run.access.status().revision, check = config => run.access.test({ config, baseRevision: base }, run.context);
  // The server's own address answers only on the port Docker publishes.
  await assert.rejects(check(policy({ http: [lan, 'http://192.0.2.10:8080'] })), refused(400, 'server_access_published_port_conflict'));
  // Another machine's port (a router maps it) is not this server's to judge.
  await check(policy({ http: [lan, 'http://203.0.113.7:8080'] }));
  await assert.rejects(check(policy({ http: [] })), refused(503, 'server_access_last_management_path'));

  // Published on loopback only: an address for the network cannot be reached.
  const loopback = start(await volume(t), { env: { MULTITAP_PUBLISHED_PORT: '127.0.0.1:18480', MULTITAP_PUBLIC_ORIGIN: secure, MULTITAP_HTTP_ORIGIN: '', MULTITAP_ALLOW_INSECURE_HTTP: '0' } });
  await loopback.access.initialize();
  assert.equal(loopback.access.status().publicIp.offered, false);
  const secured = loopback.access.status().revision;
  await assert.rejects(loopback.access.test({ config: policy({ https: [secure] }), baseRevision: secured }, loopback.context), refused(400, 'server_access_published_loopback_conflict'));
  await loopback.access.test({ config: policy({ http: [], https: [secure, 'https://desk.example.com'] }), baseRevision: secured }, loopback.context);

  // compose.direct-https.yaml: one listener, the broker's own TLS, no HTTP beside it.
  const direct = start(await volume(t), { env: { MULTITAP_TLS_MODE: 'direct', MULTITAP_TLS_CERT_FILE: '/etc/multitap/tls/cert.pem', MULTITAP_TLS_KEY_FILE: '/etc/multitap/tls/key.pem', MULTITAP_PUBLIC_ORIGIN: secure, MULTITAP_HTTP_ORIGIN: '', MULTITAP_ALLOW_INSECURE_HTTP: '0' } });
  await direct.access.initialize();
  const own = direct.access.status();
  assert.equal(own.fixed.httpsMode, 'direct'); assert.equal(own.fixed.certFile, '/etc/multitap/tls/cert.pem'); assert.equal(own.publicIp.offered, false);
  await assert.rejects(direct.access.test({ config: policy({ https: [secure] }), baseRevision: own.revision }, direct.context), refused(400, 'server_access_direct_tls_http_conflict'));
  // Whatever the page sends for the mode and the certificate, the container's are saved.
  await direct.change(policy({ http: [], https: ['https://desk.example.com'] }));
  const saved = JSON.parse(await readFile(join(direct.settings.MULTITAP_DATA_DIR, 'access', 'policy.json'), 'utf8')).config.https;
  assert.deepEqual([saved.mode, saved.certFile, saved.keyFile, saved.listenPort], ['direct', '/etc/multitap/tls/cert.pem', '/etc/multitap/tls/key.pem', 18480]);
});

test('following the public IP: turned on by a test and a save without a restart, then one address registered and replaced through the same journaled change', async t => {
  const directory = await volume(t);
  let ip = '203.0.113.7';
  const first = start(directory, { lookup: async () => ip });
  await first.access.initialize();
  const off = first.access.status();
  assert.deepEqual(off.publicIp, { offered: true, follow: false, origin: null });
  await first.access.followPublicIp();
  assert.equal(first.lookups, 0, 'nothing is asked while following is off');

  // Only the switch changes: nobody is disconnected and nothing restarts.
  const turnedOn = await first.change(policy({ follow: true }));
  assert.equal(first.exits, 0); assert.equal(first.ended, 0);
  const on = first.access.status();
  assert.equal(on.state, 'succeeded'); assert.equal(on.operationId, turnedOn); assert.notEqual(on.revision, off.revision, 'the page sees a change');
  assert.equal(on.config.http.followPublicIp, true); assert.deepEqual(on.publicIp, { offered: true, follow: true, origin: null });
  assert.deepEqual(JSON.parse(await readFile(join(directory, 'access', 'policy.json'), 'utf8')).publicIp, { follow: true, origin: null });

  // The first round: the public address becomes the last extra HTTP address.
  await first.access.followPublicIp();
  assert.equal(first.lookups, 1); assert.equal(first.exits, 1); assert.equal(first.held.length, 1);
  assert.equal(first.held[0].event, 'server_access_connections_ended'); assert.equal(first.held[0].observe(), false, 'the fence stays while it is applied');
  assert.deepEqual(first.logs, [{ service: 'multitap-broker', event: 'public_ip_address_changed', from: null, to: 'http://203.0.113.7:18480' }]);
  const second = start(directory, { lookup: async () => ip });
  assert.deepEqual(second.config.allowedOrigins, [lan, 'http://203.0.113.7:18480']);
  await second.access.initialize();
  assert.equal(second.access.startupOperationId, first.held[0].operationId);
  await second.access.resume();
  assert.deepEqual(second.probes, [lan], 'the public address is an extra one: not asked');
  const registered = second.access.status();
  assert.equal(registered.state, 'succeeded'); assert.deepEqual(registered.publicIp, { offered: true, follow: true, origin: 'http://203.0.113.7:18480' });
  assert.deepEqual(registered.config.http, { enabled: true, origin: lan, extraOrigins: ['http://203.0.113.7:18480'], listenHost: '0.0.0.0', listenPort: 18480, followPublicIp: true });
  assert.notEqual(registered.revision, second.config.accessRevision, 'the health revision is the policy\'s own');

  // The same IP: asked, nothing changes.
  await second.access.followPublicIp();
  assert.equal(second.lookups, 1); assert.equal(second.exits, 0); assert.equal(second.held.length, 0);
  // No answer, or one that is no public address: the registered address stays.
  for (const answer of [null, '10.0.0.5']) { ip = answer; await second.access.followPublicIp(); }
  assert.equal(second.lookups, 3); assert.equal(second.exits, 0); assert.equal(second.held.length, 0);
  assert.deepEqual(second.access.status().publicIp.origin, 'http://203.0.113.7:18480');

  // The provider changes the IP: that one address is replaced; the others stay in their order.
  await second.change(policy({ http: [lan, extra, 'http://203.0.113.7:18480'], follow: true }));
  const third = start(directory, { lookup: async () => ip }); await third.access.initialize(); await third.access.resume();
  assert.deepEqual(third.config.allowedOrigins, [lan, extra, 'http://203.0.113.7:18480']);
  ip = '198.51.100.20';
  await third.access.followPublicIp();
  assert.equal(third.exits, 1);
  assert.deepEqual(third.logs, [{ service: 'multitap-broker', event: 'public_ip_address_changed', from: 'http://203.0.113.7:18480', to: 'http://198.51.100.20:18480' }]);
  const fourth = start(directory, { lookup: async () => ip }); await fourth.access.initialize(); await fourth.access.resume();
  assert.deepEqual(fourth.config.allowedOrigins, [lan, extra, 'http://198.51.100.20:18480']);
  assert.deepEqual(fourth.access.status().publicIp, { offered: true, follow: true, origin: 'http://198.51.100.20:18480' });
  assert.equal((await journal(directory)).kind, 'public-ip');

  // Turned off: the address stays as an ordinary one, and nothing is asked any more.
  await fourth.change(policy({ http: [lan, extra, 'http://198.51.100.20:18480'], follow: false }));
  assert.equal(fourth.exits, 0, 'no restart for the switch alone');
  const stopped = fourth.access.status();
  assert.deepEqual(stopped.publicIp, { offered: true, follow: false, origin: null }); assert.equal(Object.hasOwn(stopped.config.http, 'followPublicIp'), false);
  assert.deepEqual(stopped.config.http.extraOrigins, [extra, 'http://198.51.100.20:18480']); assert.equal(stopped.revision, fourth.config.accessRevision);
  assert.equal(Object.hasOwn(JSON.parse(await readFile(join(directory, 'access', 'policy.json'), 'utf8')), 'publicIp'), false);
  ip = '203.0.113.99';
  const asked = fourth.lookups;
  await fourth.access.followPublicIp(); await fourth.access.followPublicIp();
  assert.equal(fourth.lookups, asked); assert.equal(fourth.exits, 0);
  const fifth = start(directory, { lookup: async () => ip }); await fifth.access.initialize(); await fifth.access.followPublicIp();
  assert.equal(fifth.lookups, 0); assert.deepEqual(fifth.config.allowedOrigins, [lan, extra, 'http://198.51.100.20:18480']);
});

test('following the public IP is offered only where it can work, never takes the main address, and does not repeat a registration that was put back', async t => {
  // Published on loopback only: not offered, refused, never asked.
  const loopback = start(await volume(t), { env: { MULTITAP_PUBLISHED_PORT: '127.0.0.1:18480' }, lookup: async () => '203.0.113.7' });
  await loopback.access.initialize();
  assert.deepEqual(loopback.access.status().publicIp, { offered: false, follow: false, origin: null });
  await assert.rejects(loopback.access.test({ config: policy({ follow: true }), baseRevision: loopback.access.status().revision }, loopback.context), refused(400, 'server_access_public_ip_conflict'));
  await loopback.access.followPublicIp(); assert.equal(loopback.lookups, 0);
  // Without HTTP there is nothing to follow with; a switch that is no boolean is no policy.
  const run = start(await volume(t), { lookup: async () => '203.0.113.7' });
  await run.access.initialize();
  const base = run.access.status().revision;
  await assert.rejects(run.access.test({ config: { ...policy({ http: [], https: [secure] }), http: { enabled: false, origin: null, listenHost: '0.0.0.0', listenPort: 18481, followPublicIp: true } }, baseRevision: base }, run.context), refused(400, 'server_access_public_ip_conflict'));
  await assert.rejects(run.access.test({ config: policy({ follow: 'yes' }), baseRevision: base }, run.context), refused(400, 'server_access_invalid'));

  // The public address is already the only HTTP address: it cannot also be the followed one.
  const only = start(await volume(t), { env: { MULTITAP_HTTP_ORIGIN: 'http://203.0.113.7:18480' }, lookup: async () => '203.0.113.7' });
  await only.access.initialize(); await only.change(policy({ http: ['http://203.0.113.7:18480'], follow: true }));
  await only.access.followPublicIp();
  assert.equal(only.exits, 0); assert.equal(only.access.status().publicIp.errorCode, 'server_access_public_ip_conflict');
  // Four addresses already: no room, and it says so.
  const full = start(await volume(t), { env: { MULTITAP_HTTP_ORIGIN: [lan, 'http://a.example.com:18480', 'http://b.example.com:18480', 'http://c.example.com:18480'].join(',') }, lookup: async () => '203.0.113.7' });
  await full.access.initialize(); await full.change(policy({ http: [lan, 'http://a.example.com:18480', 'http://b.example.com:18480', 'http://c.example.com:18480'], follow: true }));
  await full.access.followPublicIp();
  assert.equal(full.exits, 0); assert.equal(full.access.status().publicIp.errorCode, 'server_access_http_addresses_full');

  // A registration whose restart did not reach the main address goes back, and that same address is not tried again.
  const directory = await volume(t), first = start(directory, { lookup: async () => '203.0.113.7' });
  await first.access.initialize(); await first.change(policy({ follow: true })); await first.access.followPublicIp();
  assert.equal(first.exits, 1);
  const second = start(directory, { healthy: false, lookup: async () => '203.0.113.7' }); await second.access.initialize(); await second.access.resume();
  const third = start(directory, { lookup: async () => '203.0.113.7' }); await third.access.initialize();
  assert.equal(third.access.status().state, 'rolled_back'); assert.deepEqual(third.access.status().publicIp, { offered: true, follow: true, origin: null });
  await third.access.followPublicIp(); await third.access.followPublicIp();
  assert.equal(third.lookups, 2); assert.equal(third.exits, 0, 'no restart every ten minutes');
  // While a save waits, a round does nothing; `access-cli.mjs public-ip off` is seen before anything is asked.
  const raw = JSON.parse(await readFile(join(directory, 'access', 'policy.json'), 'utf8')); delete raw.publicIp;
  await writeFile(join(directory, 'access', 'policy.json'), JSON.stringify(raw));
  await third.access.followPublicIp();
  assert.equal(third.lookups, 2); assert.equal(third.access.status().publicIp.follow, false);
});

test('the public IP is asked of the address echo the multitap command uses, and only a public IPv4 counts', async () => {
  for (const value of ['203.0.113.7', '8.8.8.8', '172.15.0.1', '172.32.0.1', '100.63.0.1', '100.128.0.1']) assert.equal(publicIpv4(value), value, value);
  for (const value of ['10.1.2.3', '127.0.0.1', '0.1.2.3', '172.16.0.1', '172.31.255.1', '192.168.0.10', '100.64.0.1', '100.127.0.1', '169.254.1.1', '2001:db8::1', '300.1.1.1', '1.2.3', '', null, 'example.com']) assert.equal(publicIpv4(value), null, String(value));
  const calls = [];
  const fetcher = body => async (url, options) => { calls.push({ url, redirect: options.redirect, signal: !!options.signal }); if (body instanceof Error) throw body; return { ok: body !== null, text: async () => body }; };
  assert.equal(await lookupPublicIp({ fetcher: fetcher('fl=1\nh=1.1.1.1\nip=203.0.113.7\nts=1\n') }), '203.0.113.7');
  assert.deepEqual(calls, [{ url: 'https://1.1.1.1/cdn-cgi/trace', redirect: 'error', signal: true }]);
  for (const body of ['ip=192.168.0.10\n', 'ip=2001:db8::1\n', 'nothing', '', null, Error('offline')]) assert.equal(await lookupPublicIp({ fetcher: fetcher(body) }), null);
});

test('the real server: the console\'s test and save reach the in-process client through the same fence, with the server\'s own activity report', { timeout: 20000 }, async t => {
  const directory = await volume(t), run = start(directory, { env: { MULTITAP_HOST: '127.0.0.1' } });
  await run.access.initialize();
  const store = new Store(':memory:'), config = run.config;
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: config.publicOrigin, allowInsecureHttp: config.allowInsecureHttp, accessConfig: config.accessConfig, accessRevision: config.accessRevision,
    accessInstallationId: config.accessInstallationId, accessIdentityFile: null, accessClient: run.access, deploymentKind: 'docker', trustedProxies: config.trustedProxies, initialSetupOptions: { announce: () => {} }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const runtime = server.accessRuntime, context = { managementOrigin: lan };
  const status = await runtime.status(context);
  assert.equal(status.supported, true, 'the page shows its editor, not a command to copy'); assert.equal(status.selfManaged, true); assert.equal(status.managementOrigin, lan);
  assert.deepEqual(status.applied, config.accessConfig); assert.equal(status.fixed.listenPort, 18480);
  const next = policy({ http: [lan, extra] });
  const tested = await runtime.test({ config: next, baseRevision: status.revision }, context);
  const saved = await runtime.save({ config: next, testId: tested.testId, baseRevision: status.revision, confirm: true }, context, { actor: 'owner' });
  assert.equal(saved.state, 'waiting_idle'); assert.equal(runtime.blocked, true);
  await run.access.task;
  assert.equal(run.exits, 1, 'the real activity report let it through');
  assert.equal((await runtime.status(context)).pending.state, 'applying');
  assert.equal(runtime.blocked, true, 'closed until the restarted server knows the result');
  assert.deepEqual(JSON.parse(await readFile(join(directory, 'access', 'policy.json'), 'utf8')).config.http.extraOrigins, [extra]);
});

test('the real server: a round of the public IP takes the fence a save takes and gives it back when nothing was applied', { timeout: 20000 }, async t => {
  const directory = await volume(t), prepared = start(directory, { env: { MULTITAP_HOST: '127.0.0.1' } });
  await prepared.access.initialize(); await prepared.change(policy({ follow: true }));
  let updates = 1;
  const run = start(directory, { env: { MULTITAP_HOST: '127.0.0.1' }, lookup: async () => '203.0.113.7', maxWaitMs: 0, sleep: () => new Promise(resolve => setImmediate(resolve)) });
  await run.access.initialize();
  const store = new Store(':memory:'), config = run.config;
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: config.publicOrigin, allowInsecureHttp: config.allowInsecureHttp, accessConfig: config.accessConfig, accessRevision: config.accessRevision,
    accessInstallationId: config.accessInstallationId, accessIdentityFile: null, accessClient: run.access, deploymentKind: 'docker', trustedProxies: config.trustedProxies, initialSetupOptions: { announce: () => {} }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const runtime = server.accessRuntime, hooks = run.access.hooks;
  // The server's own work is in the way and the wait runs out: nothing was applied.
  run.access.attach({ ...hooks, activity: () => { const report = hooks.activity(); report.activity.operations.hostUpdates = updates; return report; } });
  await run.access.followPublicIp();
  assert.equal(run.exits, 0); assert.equal(run.access.status().state, 'failed'); assert.equal(run.access.status().errorCode, 'server_access_idle_timeout');
  assert.equal(runtime.blocked, true, 'held until the runtime reads the end');
  await new Promise(resolve => setTimeout(resolve, 2300));
  assert.equal(runtime.blocked, false);
  // The next round applies it.
  updates = 0;
  await run.access.followPublicIp();
  assert.equal(run.exits, 1); assert.equal(runtime.blocked, true);
  assert.deepEqual(JSON.parse(await readFile(join(directory, 'access', 'policy.json'), 'utf8')).publicIp, { follow: true, origin: 'http://203.0.113.7:18480' });
});

test('the container\'s default gateway is read from the kernel\'s route table and offered only on the network compose makes', async t => {
  const table = 'Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n' +
    'eth0\t00000000\t010012AC\t0003\t0\t0\t0\t00000000\t0\t0\t0\n' + 'eth0\t000012AC\t00000000\t0001\t0\t0\t0\t0000FFFF\t0\t0\t0\n';
  assert.equal(defaultGateway(() => table), '172.18.0.1');
  // Another default route first, in the kernel's order: that one.
  assert.equal(defaultGateway(() => table.replace('eth0\t00000000\t010012AC', 'eth1\t00000000\t0100A8C0\t0003\t0\t0\t0\t00000000\t0\t0\t0\neth0\t00000000\t010012AC')), '192.168.0.1');
  // No default route, one that is down or has no gateway, a table that cannot be read, or a system without one.
  for (const text of ['Iface\tDestination\tGateway\n', table.replace('\t0003\t', '\t0001\t'), table.replace('\t0003\t', '\t0002\t'), table.replace('010012AC', '00000000'), table.replace('010012AC', 'zz0012AC'), '']) assert.equal(defaultGateway(() => text), null, JSON.stringify(text.slice(0, 60)));
  assert.equal(defaultGateway(() => { throw Error('EACCES'); }), null); assert.equal(defaultGateway(null), null);

  let reads = 0;
  const run = start(await volume(t), { gateway: () => { reads++; return '172.18.0.1'; } });
  await run.access.initialize();
  assert.equal(run.access.status().gateway, '172.18.0.1'); assert.equal(run.access.status().gateway, '172.18.0.1'); assert.equal(reads, 1, 'read once, at the start');
  assert.deepEqual(run.access.status().config.trustedProxies, ['127.0.0.1', '::1'], 'a suggestion for the page, never a trusted proxy by itself');
  // Without the published port compose passes, this is not known to be that network.
  const other = start(await volume(t), { env: { MULTITAP_PUBLISHED_PORT: '', MULTITAP_SERVER_ADDRESS: '' }, gateway: () => { reads++; return '192.168.0.1'; } });
  await other.access.initialize();
  assert.equal(other.access.status().gateway, null); assert.equal(reads, 1);
});

test('the .env address lines are "ignored" only once somebody edits them after a save; the lines a policy was saved beside are its seed', async t => {
  const directory = await volume(t), first = start(directory);
  await first.access.initialize();
  assert.equal(first.access.status().envAddressesIgnored, false, 'nothing saved: the lines are in use');
  // A fresh install: the first save from the console. The lines installed with it still stand in .env.
  await first.change(policy({ http: [lan, extra] }));
  for (let index = 0; index < 2; index++) {
    const again = start(directory); await again.access.initialize(); await again.access.resume();
    assert.equal(again.config.selfAccess.envAddressesIgnored, false, 'every later start: no warning'); assert.equal(again.access.status().envAddressesIgnored, false);
    assert.deepEqual(again.config.allowedOrigins, [lan, extra]);
  }
  // Somebody edits .env and expects it to work: said at the start, on the page and by the command.
  const edited = { MULTITAP_HTTP_ORIGIN: 'http://192.0.2.50:18480' };
  const second = start(directory, { env: edited }); await second.access.initialize();
  assert.equal(second.config.selfAccess.envAddressesIgnored, true); assert.equal(second.access.status().envAddressesIgnored, true);
  assert.deepEqual(second.config.allowedOrigins, [lan, extra], 'the saved policy still decides');
  // An administrator saves beside the edited lines: they are known now.
  await second.change(policy({ http: [lan, extra], follow: true }));
  assert.equal(second.access.status().envAddressesIgnored, false);
  const third = start(directory, { env: edited, lookup: async () => '203.0.113.7' }); await third.access.initialize();
  assert.equal(third.config.selfAccess.envAddressesIgnored, false);
  // The server's own change of the public address keeps what was remembered: a later edit is still said.
  await third.access.followPublicIp();
  assert.equal(third.exits, 1);
  const moved = { MULTITAP_HTTP_ORIGIN: 'http://192.0.2.60:18480' };
  const fourth = start(directory, { env: moved }); await fourth.access.initialize(); await fourth.access.resume();
  assert.equal(fourth.config.selfAccess.envAddressesIgnored, true, 'edited again after the last save');
  assert.equal(JSON.parse(await readFile(join(directory, 'access', 'policy.json'), 'utf8')).envSeed, third.config.selfAccess.envSeed, 'the registration did not adopt lines no administrator saved beside');
  // Emptied lines (the multitap command does that once a policy is saved) are nothing to ignore.
  const emptied = start(directory, { env: { MULTITAP_HTTP_ORIGIN: '', MULTITAP_ALLOW_INSECURE_HTTP: '0' } }); await emptied.access.initialize();
  assert.equal(emptied.config.selfAccess.envAddressesIgnored, false); assert.equal(emptied.config.selfAccess.envSeed, null);
  // Lines edited to the very addresses of the policy change nothing either.
  const same = start(directory, { env: { MULTITAP_HTTP_ORIGIN: [lan, extra, 'http://203.0.113.7:18480'].join(',') } }); await same.access.initialize();
  assert.equal(same.config.selfAccess.envAddressesIgnored, false);
});

// ---- the volume changes under a running server (access-cli.mjs from another container) ----
// The container's health check, as update/server-access-health.mjs
// --container-health judges the server: the same checks in the same order,
// with this test's file reader. It loads the configuration from the volume
// and asks the running server at its own listener.
async function containerHealthy(settings) {
  let current;
  try { current = configuration(settings, { readAccessFile }); } catch { return false; }
  const transport = current.accessConfig ? serverAccessTransport(current.accessConfig) : { publicOrigin: null, tlsMode: 'proxy', tlsFiles: null, bootstrapOnly: true, port: current.bootstrapPort };
  const localTransport = { ...transport, host: '127.0.0.1' };
  const value = await localAccessRequest(localTransport);
  if (value?.service !== 'multitap-broker' || value.status !== 'ok' || value.authentication !== 'id-password' || value.version !== VERSION || value.access?.protocol !== 'server-access-v1' || value.access.revision !== current.accessRevision) return false;
  if (!current.accessConfig && !validBootstrapLocalHealth(value, localTransport)) return false;
  if (current.accessInstallationId && !validAccessHealth(value, { config: current.accessConfig, revision: current.accessRevision, installationId: current.accessInstallationId, origin: current.accessConfig?.primaryOrigin ?? bootstrapHealthOrigin(localTransport), localTransport, allowHealthProbeOnly: true })) return false;
  return true;
}
const freePort = () => new Promise(resolve => { const probe = createServer(); probe.listen(0, '127.0.0.1', () => { const port = probe.address().port; probe.close(() => resolve(port)); }); });
// One running server on the volume, as index.mjs builds it, and the command line beside it.
async function running(t, directory, port, options = {}) {
  const here = 'http://192.0.2.10:' + port, outside = 'http://203.0.113.7:' + port;
  const run = start(directory, { ...options, env: { MULTITAP_HOST: '127.0.0.1', MULTITAP_PORT: String(port), MULTITAP_PUBLISHED_PORT: '0.0.0.0:' + port, MULTITAP_HTTP_ORIGIN: here + ',' + outside, ...options.env } });
  await run.access.initialize();
  const store = new Store(':memory:'), config = run.config;
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: config.publicOrigin, allowInsecureHttp: config.allowInsecureHttp, accessConfig: config.accessConfig, accessRevision: config.accessRevision,
    accessInstallationId: config.accessInstallationId, accessIdentityFile: config.accessIdentityFile, accessIdentityRead: () => config.selfAccess.read(config.accessIdentityFile, 128), accessClient: run.access,
    bootstrap: config.bootstrapOnly, setupOrigin: config.bootstrapOnly ? config.setupOrigin : null, deploymentKind: 'docker', trustedProxies: config.trustedProxies, initialSetupOptions: { announce: () => {} }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  const stop = async () => { if (!server.listening) return; server.closeAllConnections(); await new Promise(resolve => server.close(resolve)); store.close(); };
  t.after(stop);
  const cli = async (...argv) => {
    let text = '';
    const code = await accessCli(argv, { env: run.settings, stdout: { write: chunk => { text += chunk; } }, configure: value => configuration(value, { readAccessFile }), storage: value => selfAccessStorage(value, { readAccessFile }) });
    return { code, values: Object.fromEntries(text.split('\n').filter(Boolean).map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)])) };
  };
  const healthz = () => localAccessRequest({ ...serverAccessTransport(config.accessConfig), host: '127.0.0.1' });
  return { ...run, run, server, stop, cli, healthz, healthy: () => containerHealthy(run.settings), here, outside, status: () => server.accessRuntime.status({ managementOrigin: here }) };
}

test('a first policy written by the command line under a running server: the health check keeps passing, the server takes it over and nobody is disconnected', { timeout: 30000 }, async t => {
  const directory = await volume(t), port = await freePort();
  // A server on its .env lines (an install updated from 1.1.1): no access folder, no installation ID.
  const f = await running(t, directory, port, { lookup: async () => '203.0.113.7' });
  assert.equal(f.config.selfAccess.saved, false); assert.equal((await f.healthz()).access.installationId, null);
  assert.equal(await f.healthy(), true);
  await assert.rejects(readdir(join(directory, 'access')), { code: 'ENOENT' });
  // The host's timer: multitap public-ip sync -> access-cli public-ip on --address <ip>.
  const on = await f.cli('public-ip', 'on', '--address', '203.0.113.7');
  assert.equal(on.code, 0); assert.equal(on.values.changed, '1'); assert.equal(on.values.source, 'saved'); assert.equal(on.values.followed, f.outside);
  assert.deepEqual((await readdir(join(directory, 'access'))).sort(), ['identity', 'policy.before-cli.json', 'policy.json']);
  const identity = (await readFile(join(directory, 'access', 'identity'), 'utf8')).trim();
  // At once, before the server has looked at the volume: its answer names the installation the volume now has.
  assert.equal(await f.healthy(), true, 'the container stays healthy');
  const answer = await f.healthz();
  assert.equal(answer.access.installationId, identity); assert.equal(answer.access.revision, f.config.accessRevision);
  for (let index = 0; index < 3; index++) assert.equal(await f.healthy(), true);
  // The server's look at the volume: the same addresses, so it takes the rest over.
  assert.equal((await f.status()).applied.http.followPublicIp, undefined, 'not yet');
  await f.access.reconcile();
  const status = await f.status();
  assert.equal(status.legacy, false); assert.deepEqual(status.publicIp, { offered: true, follow: true, origin: f.outside }); assert.equal(status.applied.http.followPublicIp, true);
  assert.equal(status.revision, on.values.revision, 'the page and the command see one policy');
  assert.equal(f.run.exits, 0); assert.equal(f.server.accessRuntime.blocked, false, 'no restart, no fence, nobody disconnected');
  assert.equal(await f.healthy(), true);
  assert.equal(f.access.followDue, true, 'turned on from the command line: the first round is due now, not in ten minutes');
  // Its rounds follow the public IP from now on: the registered address is the current one.
  await f.access.followPublicIp();
  assert.equal(f.run.lookups, 1); assert.equal(f.run.exits, 0); assert.equal(f.access.followDue, false);
  // A save from the console right afterwards works on what was taken over.
  const base = (await f.status()).revision, next = { ...status.applied, trustedProxies: ['172.18.0.1'] };
  const tested = await f.server.accessRuntime.test({ config: next, baseRevision: base }, { managementOrigin: f.here });
  await f.server.accessRuntime.save({ config: next, testId: tested.testId, baseRevision: base, confirm: true }, { managementOrigin: f.here }, { actor: 'owner' });
  await f.access.task;
  assert.equal(f.run.exits, 1); assert.equal((await readFile(join(directory, 'access', 'identity'), 'utf8')).trim(), identity, 'the installation keeps its ID');

  // off under a running server: taken over the same way, and nothing is asked any more.
  const g = await running(t, await volume(t), await freePort(), { lookup: async () => '203.0.113.7' });
  await g.cli('public-ip', 'on', '--address', '203.0.113.7'); await g.access.reconcile();
  assert.equal((await g.cli('public-ip', 'off')).values.changed, '1');
  assert.equal(await g.healthy(), true);
  await g.access.followPublicIp();
  assert.equal(g.run.lookups, 0); assert.equal((await g.status()).publicIp.follow, false); assert.equal(g.run.exits, 0); assert.equal(await g.healthy(), true);
});

test('other addresses written under a running server: it ends the connections and starts again by itself, once; set, reset and revert alike', { timeout: 30000 }, async t => {
  const directory = await volume(t), port = await freePort();
  const f = await running(t, directory, port);
  const hooks = f.access.hooks; let ended = 0;
  f.access.attach({ ...hooks, endConnections: () => { ended++; return hooks.endConnections(); } });
  // set, and nobody starts the server again.
  const set = await f.cli('set', f.here, 'http://vdi.example.com:' + port);
  assert.equal(set.values.changed, '1');
  assert.equal(await f.healthy(), false, 'the volume names addresses this process does not serve');
  await f.access.reconcile();
  assert.equal(f.run.exits, 1, 'it starts again to serve them'); assert.equal(ended, 1); assert.equal(f.server.accessRuntime.blocked, true, 'closed like a save until it has ended');
  assert.deepEqual(f.run.logs.map(line => line.event), ['server_access_policy_changed']);
  await f.access.reconcile(); await f.access.followPublicIp();
  assert.equal(f.run.exits, 1, 'asked once');
  await f.stop();
  // The process that starts in its place serves them; nothing was in flight, so there is nothing to check or undo.
  const second = await running(t, directory, port, { env: {} });
  assert.deepEqual(second.config.allowedOrigins, [f.here, 'http://vdi.example.com:' + port]);
  assert.equal(second.access.startupOperationId, null); assert.equal(await second.healthy(), true);
  assert.equal((await second.healthz()).access.installationId, second.config.accessInstallationId);
  await second.access.reconcile(); assert.equal(second.run.exits, 0, 'the volume and the server agree');

  // revert before the server looked: the policy it runs is back, so nothing happens.
  await second.cli('set', f.here); assert.equal(await second.healthy(), false);
  assert.equal((await second.cli('revert')).code, 0);
  assert.equal(await second.healthy(), true);
  await second.access.reconcile(); assert.equal(second.run.exits, 0);
  // reset: the .env lines apply again, and they name other addresses than the policy did.
  assert.equal((await second.cli('reset')).values.source, 'env');
  assert.equal(await second.healthy(), false);
  await second.access.reconcile();
  assert.equal(second.run.exits, 1);
  await second.stop();
  const third = await running(t, directory, port, { env: {} });
  assert.deepEqual(third.config.allowedOrigins, [f.here, f.outside]); assert.equal(third.config.selfAccess.saved, false);
  assert.equal(await third.healthy(), true, 'on its .env lines again, with the ID it keeps');
  assert.match((await third.healthz()).access.installationId, /^[a-f0-9]{64}$/);
});

test('the look at the volume leaves a save in flight, the server\'s own unfinished work and an unreadable policy alone', { timeout: 30000 }, async t => {
  const directory = await volume(t), port = await freePort();
  let updates = 0;
  const f = await running(t, directory, port, { sleep: () => new Promise(resolve => setImmediate(resolve)) });
  const hooks = f.access.hooks;
  f.access.attach({ ...hooks, activity: () => { const report = hooks.activity(); report.activity.operations.hostUpdates = updates; return report; } });
  // A Host update is running: the restart for the new addresses waits for it.
  await f.cli('set', f.here, 'http://vdi.example.com:' + port);
  updates = 1;
  await f.access.reconcile();
  assert.equal(f.run.exits, 0); assert.equal(f.server.accessRuntime.blocked, false);
  // A policy that does not load is not started into: the health check keeps failing instead.
  updates = 0;
  await writeFile(join(directory, 'access', 'policy.json'), 'not a policy');
  await f.access.reconcile();
  assert.equal(f.run.exits, 0); assert.equal(await f.healthy(), false);
  // While a save of this server waits or is being applied, the files are that save's.
  await (await f.cli('reset')).code;
  updates = 1;
  const status = await f.status(), next = { ...status.applied, trustedProxies: ['172.18.0.1'] };
  const tested = await f.server.accessRuntime.test({ config: next, baseRevision: status.revision }, { managementOrigin: f.here });
  const saved = await f.server.accessRuntime.save({ config: next, testId: tested.testId, baseRevision: status.revision, confirm: true }, { managementOrigin: f.here }, { actor: 'owner' });
  await new Promise(resolve => setTimeout(resolve, 20));
  assert.equal((await f.status()).pending.state, 'waiting_idle');
  await f.access.reconcile(); await f.access.followPublicIp();
  assert.equal(f.run.exits, 0);
  await f.server.accessRuntime.cancel(saved.operationId);
  assert.equal(f.run.exits, 0);
});

// ---- following turned on: the first lookup, with the real fence of the save ----
const eventually = async (predicate, ms = 5000) => { for (const end = Date.now() + ms; !predicate();) { assert.ok(Date.now() < end, 'not within ' + ms + ' ms'); await new Promise(resolve => setTimeout(resolve, 10)); } };
async function turnOn(f) {
  const status = await f.status(), next = { ...status.applied, http: { ...status.applied.http, followPublicIp: true } };
  const tested = await f.server.accessRuntime.test({ config: next, baseRevision: status.revision }, { managementOrigin: f.here });
  await f.server.accessRuntime.save({ config: next, testId: tested.testId, baseRevision: status.revision, confirm: true }, { managementOrigin: f.here }, { actor: 'owner' });
  await f.access.task;
}

test('following turned on in the console is registered at once: the save gives its fence back and the first lookup runs right after it', { timeout: 30000 }, async t => {
  const f = await running(t, await volume(t), await freePort(), { lookup: async () => '203.0.113.7' });
  await turnOn(f);
  // No status read by anybody and no ten-minute round: the address is there within moments.
  await eventually(() => f.access.status().publicIp.origin === f.outside, 1500);
  assert.equal(f.run.lookups, 1); assert.equal(f.run.exits, 0, 'the address was already served: nobody is disconnected');
  assert.deepEqual(f.run.logs, [{ service: 'multitap-broker', event: 'public_ip_address_changed', from: null, to: f.outside }]);
  await eventually(() => !f.server.accessRuntime.blocked, 3000);
  assert.deepEqual((await f.status()).publicIp, { offered: true, follow: true, origin: f.outside });
  assert.equal(f.access.followDue, false);
  assert.deepEqual(JSON.parse(await readFile(join(f.settings.MULTITAP_DATA_DIR, 'access', 'policy.json'), 'utf8')).publicIp, { follow: true, origin: f.outside });
});

test('a first lookup that cannot start behind a fence asks nothing meanwhile and is tried again in seconds, not at the next ten-minute round', { timeout: 30000 }, async t => {
  // The fence of the save is given back only by the runtime's own look (every 2 s), as when nothing tells it sooner.
  const f = await running(t, await volume(t), await freePort(), { lookup: async () => '203.0.113.7', followRetryMs: 100 });
  const { settle: _dropped, ...hooks } = f.access.hooks;
  f.access.attach(hooks);
  await turnOn(f);
  await new Promise(resolve => setTimeout(resolve, 300));
  assert.equal(f.server.accessRuntime.blocked, true, 'still the fence of the save');
  assert.equal(f.run.lookups, 0, 'nothing is asked that could not be applied'); assert.equal(f.access.followDue, true, 'and nothing is dropped');
  assert.equal(f.access.status().publicIp.origin, null);
  await eventually(() => f.access.status().publicIp.origin === f.outside, 5000);
  assert.equal(f.run.lookups, 1);

  // The fence is taken between the lookup and the registration (a save arrives): due again, and done once it is free.
  let release, asked = 0;
  const g = await running(t, await volume(t), await freePort(), { followRetryMs: 100, lookup: () => ++asked > 1 ? Promise.resolve('203.0.113.7') : new Promise(resolve => { release = () => resolve('203.0.113.7'); }) });
  await turnOn(g);
  await eventually(() => typeof release === 'function', 1500);
  const lease = g.server.accessRuntime.hold({ operationId: randomUUID(), event: 'fixture', observe: () => false });
  release(); await new Promise(resolve => setTimeout(resolve, 150));
  assert.equal(g.access.status().publicIp.origin, null); assert.equal(g.access.followDue, true);
  g.server.accessRuntime.release(lease);
  await eventually(() => g.access.status().publicIp.origin === g.outside, 3000);

  // A lookup that fails outright is tried again soon as well.
  let calls = 0;
  const h = await running(t, await volume(t), await freePort(), { followRetryMs: 100, lookup: async () => { if (++calls === 1) throw Error('offline'); return '203.0.113.7'; } });
  await turnOn(h);
  await eventually(() => h.access.status().publicIp.origin === h.outside, 3000);
  assert.equal(calls, 2);
});

test('following turned on from the command line is looked up at the server\'s next look at the volume', { timeout: 30000 }, async t => {
  const f = await running(t, await volume(t), await freePort(), { lookup: async () => '198.51.100.20' });
  assert.equal((await f.cli('public-ip', 'on')).values.changed, '1');
  await f.access.look();
  assert.equal(f.run.lookups, 1, 'not ten minutes later');
  assert.equal(f.run.exits, 1, 'a new address: registered through the journaled change'); assert.equal(f.server.accessRuntime.blocked, true);
  assert.deepEqual(f.run.logs, [{ service: 'multitap-broker', event: 'public_ip_address_changed', from: null, to: 'http://198.51.100.20:' + f.settings.MULTITAP_PORT }]);
});

// ---- a saved policy that cannot be used ----
test('a saved policy that cannot be read: the server starts with only its console at its own address, says why, keeps the file, and an administrator saves it right again', { timeout: 30000 }, async t => {
  const directory = await volume(t), port = await freePort();
  const good = await running(t, directory, port);
  await good.cli('set', good.here, 'http://vdi.example.com:' + port);
  await good.stop();
  await writeFile(join(directory, 'access', 'policy.json'), 'not a policy');
  // The start that used to end with server_access_state_invalid, again and again.
  const f = await running(t, directory, port);
  assert.equal(f.config.selfAccess.unreadable, 'server_access_state_invalid'); assert.equal(f.config.bootstrapOnly, true);
  assert.deepEqual(f.config.allowedOrigins, [], 'never the .env lines in its place');
  const status = await f.status();
  assert.equal(status.policyUnreadable, 'server_access_state_invalid'); assert.equal(status.applied, null); assert.equal(status.supported, true);
  // Only the console, only at the server's own address (as before the first addresses): no app, Host or download.
  const ask = (host, path, headers = {}) => new Promise((resolve, reject) => { const req = httpRequest({ hostname: '127.0.0.1', port, path, headers: { Host: host, ...headers } }, res => { res.resume(); res.once('end', () => resolve(res.statusCode)); }); req.on('error', reject); req.end(); });
  assert.equal(await ask('192.0.2.10:' + port, '/admin/', { Accept: 'text/html' }), 200);
  for (const path of ['/api/v1/downloads', '/api/v1/desktops', '/download', '/get/client']) assert.equal(await ask('192.0.2.10:' + port, path), 403, path);
  for (const host of ['vdi.example.com:' + port, '203.0.113.7:' + port]) assert.equal(await ask(host, '/admin/', { Accept: 'text/html' }), 403, host);
  assert.equal(await f.healthy(), true, 'the process answers: no restart loop');
  assert.equal(await readFile(join(directory, 'access', 'policy.json'), 'utf8'), 'not a policy', 'the file is not touched by the start');
  // The command line says the reason as before, and reset is its way out.
  assert.deepEqual(await f.cli('show').then(result => [result.code, result.values.code]), [2, 'server_access_state_invalid']);
  // The administrator saves the addresses again from the console.
  const next = policy({ http: [f.here, 'http://vdi.example.com:' + port] });
  const tested = await f.server.accessRuntime.test({ config: next, baseRevision: status.revision }, { managementOrigin: f.here });
  await f.server.accessRuntime.save({ config: next, testId: tested.testId, baseRevision: status.revision, confirm: true }, { managementOrigin: f.here }, { actor: 'owner' });
  await f.access.task;
  assert.equal(f.run.exits, 1);
  assert.equal((await journal(directory)).previous, 'not a policy', 'what was there is what a failed check puts back');
  await f.stop();
  // The addresses do not answer: the unreadable file is back, and so is the start without addresses.
  const failed = start(directory, { env: { MULTITAP_HOST: '127.0.0.1', MULTITAP_PORT: String(port), MULTITAP_PUBLISHED_PORT: '0.0.0.0:' + port, MULTITAP_HTTP_ORIGIN: f.here + ',' + f.outside }, healthy: false });
  assert.equal(failed.config.selfAccess.unreadable, null); assert.deepEqual(failed.config.allowedOrigins, [f.here, 'http://vdi.example.com:' + port]);
  await failed.access.initialize(); await failed.access.resume();
  assert.equal(failed.exits, 1); assert.equal(await readFile(join(directory, 'access', 'policy.json'), 'utf8'), 'not a policy');
  const again = await running(t, directory, port);
  assert.equal(again.config.selfAccess.unreadable, 'server_access_state_invalid'); assert.equal((await again.status()).policyUnreadable, 'server_access_state_invalid');
  // reset from the command line under the running server: it starts again on the .env lines by itself.
  assert.equal((await again.cli('reset')).values.source, 'env');
  await again.access.reconcile();
  assert.equal(again.run.exits, 1);
});

test('an unreadable policy removed from the command line, with no address lines in .env: the running server stops saying it and stays as it is', { timeout: 30000 }, async t => {
  const directory = await volume(t), port = await freePort();
  // The usual state once the multitap command has emptied the .env address lines.
  const env = { MULTITAP_HTTP_ORIGIN: '', MULTITAP_ALLOW_INSECURE_HTTP: '0', NODE_ENV: 'production' };
  const good = await running(t, directory, port, { env });
  assert.equal(good.config.bootstrapOnly, true);
  await good.cli('set', good.here);
  await good.stop();
  await writeFile(join(directory, 'access', 'policy.json'), 'not a policy');
  const f = await running(t, directory, port, { env });
  assert.equal((await f.status()).policyUnreadable, 'server_access_state_invalid');
  const reset = await f.cli('reset');
  assert.equal(reset.values.changed, '1'); assert.equal(reset.values.source, 'none');
  assert.deepEqual((await readdir(join(directory, 'access'))).sort(), ['identity']);
  // The server's look at the volume: nothing to serve before and after, so nothing restarts; only the reason is gone.
  await f.access.look();
  const status = await f.status();
  assert.equal(Object.hasOwn(status, 'policyUnreadable'), false, 'no banner any more'); assert.equal(status.applied, null); assert.equal(status.supported, true);
  assert.equal(f.run.exits, 0); assert.deepEqual(f.run.logs, []); assert.equal(f.server.accessRuntime.blocked, false);
  assert.equal(await f.healthy(), true);
  // And it is the plain state before the first addresses: an administrator saves them from the console.
  const next = policy({ http: [f.here] });
  const tested = await f.server.accessRuntime.test({ config: next, baseRevision: status.revision }, { managementOrigin: f.here });
  await f.server.accessRuntime.save({ config: next, testId: tested.testId, baseRevision: status.revision, confirm: true }, { managementOrigin: f.here }, { actor: 'owner' });
  await f.access.task;
  assert.equal(f.run.exits, 1); assert.equal((await journal(directory)).previous, null, 'nothing unreadable to put back');
});

// ---- 연결 테스트 reaches the addresses before anything is saved ----------------
test('연결 테스트 tries every main address before anything is saved: one that does not lead to this server gives its reason, and no test an ordinary save can name', async t => {
  const directory = await volume(t);
  let found = true;
  const run = start(directory, { reach: (origin, nonce) => origin !== secure ? true : typeof found === 'function' ? found(nonce) : found });
  await run.access.initialize();
  const next = policy({ http: [lan], https: [secure], primaryOrigin: secure }), base = run.access.status().revision;
  const tryIt = () => run.access.test({ config: next, baseRevision: base }, run.context);
  const reached = await tryIt();
  assert.deepEqual(run.tried, [secure, lan], 'the main address of each card, never an extra one');
  assert.deepEqual(reached.checks, [{ name: 'configuration', status: 'passed' }, { name: 'managed_installation', status: 'passed' },
    { name: 'address', origin: secure, status: 'passed', code: 'server_access_address_reached' }, { name: 'address', origin: lan, status: 'passed', code: 'server_access_address_reached' }]);
  assert.match(reached.testId, /^[a-f0-9]{64}$/); assert.ok(!reached.checks.some(check => check.status === 'pending'), 'nothing is left "to be checked after applying"');

  // `outside`: the reason is also what an address that works from outside looks like from in here, so the
  // administrator may prove it from outside (the tests further down); every other reason is a fault no proof mends.
  const failing = async (answer, expected, { outside = false } = {}) => {
    found = answer;
    const result = await tryIt(), check = result.checks.find(row => row.origin === secure);
    assert.deepEqual(check, { name: 'address', origin: secure, status: 'failed', ...expected }, JSON.stringify(answer));
    assert.equal(result.checks.find(row => row.origin === lan).status, 'passed', 'the other address is still told apart');
    assert.equal(result.scope, 'preflight'); assert.equal(result.baseRevision, base); assert.match(result.configHash, /^[a-f0-9]{64}$/);
    if (!outside) { assert.equal(result.testId, undefined, 'no test to save with'); assert.equal(result.expiresAt, undefined); assert.equal(result.confirmation, undefined, 'and nothing to prove from outside'); return; }
    assert.equal(result.confirmation, 'outside'); assert.equal(result.confirmationWindowMs, 300000); assert.match(result.testId, /^[a-f0-9]{64}$/);
    await assert.rejects(run.access.save({ config: next, testId: result.testId, baseRevision: base, confirm: true }, { ...run.context, fence: { token: randomBytes(32).toString('hex'), operationId: randomUUID() } }), refused(409, 'server_access_test_required'), 'an ordinary save cannot name it');
  };
  for (const reason of ['name_unknown', 'unchecked', 'closed', 'no_answer']) await failing({ error: reason }, { code: 'server_access_address_' + reason }, { outside: true });
  for (const reason of ['not_https', 'certificate']) await failing({ error: reason }, { code: 'server_access_address_' + reason });
  await failing({ error: 'anything_else' }, { code: 'server_access_address_no_answer' }, { outside: true });
  await failing(() => { throw Error('offline'); }, { code: 'server_access_address_no_answer' }, { outside: true });
  // Something answers there, but it is not this server answering this test.
  await failing({ answer: null }, { code: 'server_access_address_other_server' });
  await failing({ answer: { service: 'multitap-broker', status: 'ok' } }, { code: 'server_access_address_other_server' });
  await failing({ answer: { protocol: accessProbeProtocol, nonce: 'f'.repeat(64), admitted: true } }, { code: 'server_access_address_other_server' });
  // This server answered, and would not let that request in.
  await failing(nonce => ({ answer: { protocol: accessProbeProtocol, nonce, admitted: false, reason: 'proxy_untrusted', peer: '172.18.0.1' } }), { peer: '172.18.0.1', code: 'server_access_address_proxy_untrusted' });
  await failing(nonce => ({ answer: { protocol: accessProbeProtocol, nonce, admitted: false, reason: 'proxy_headers', peer: '127.0.0.1' } }), { peer: '127.0.0.1', code: 'server_access_address_proxy_headers' });
  await failing(nonce => ({ answer: { protocol: accessProbeProtocol, nonce, admitted: false, reason: 'made up', peer: 'not an address' } }), { code: 'server_access_address_not_admitted' });
  await failing(nonce => ({ answer: { protocol: accessProbeProtocol, nonce, admitted: 'yes' } }), { code: 'server_access_address_not_admitted' });

  // Nothing happened to the server or to anybody connected to it.
  assert.equal(run.exits, 0); assert.equal(run.ended, 0); assert.equal(run.access.status().state, 'idle'); assert.equal(run.access.status().revision, base);
  assert.deepEqual(await readdir(directory), [], 'nothing was written');
  await assert.rejects(run.access.save({ config: next, testId: 'e'.repeat(64), baseRevision: base, confirm: true }, { ...run.context, fence: { token: randomBytes(32).toString('hex'), operationId: randomUUID() } }), refused(409, 'server_access_test_required'));
  // A test that reached it is the one an ordinary save may name.
  found = true;
  await run.access.save({ config: next, testId: (await tryIt()).testId, baseRevision: base, confirm: true }, { ...run.context, fence: { token: randomBytes(32).toString('hex'), operationId: randomUUID() } });
  await run.access.task;
  assert.equal(run.exits, 1);
});

// Round 2 of the browser check: a wrong extra address was saved silently. It is tried now, with the same reasons.
test('연결 테스트 tries every extra address too: out of reach from inside is said and saved without a proof, a fault on it blocks like on a main address', async t => {
  const directory = await volume(t), forward = 'http://203.0.113.7:18480';
  let atForward = { error: 'no_answer' };
  const run = start(directory, { reach: (origin, nonce) => origin !== forward ? true : typeof atForward === 'function' ? atForward(nonce) : atForward });
  await run.access.initialize();
  const next = policy({ http: [lan, forward] }), base = run.access.status().revision;
  const tryIt = () => run.access.test({ config: next, baseRevision: base }, run.context);
  const first = await tryIt();
  assert.deepEqual(run.tried, [lan, forward], 'the main address first, then the extra one');
  assert.deepEqual(first.checks.slice(2), [{ name: 'address', origin: lan, status: 'passed', code: 'server_access_address_reached' },
    { name: 'address', origin: forward, extra: true, status: 'unreached', code: 'server_access_address_no_answer' }]);
  assert.match(first.testId, /^[a-f0-9]{64}$/, 'a test an ordinary save can name'); assert.equal(first.confirmation, undefined, 'no proof from outside is asked for an extra address');
  for (const reason of ['name_unknown', 'unchecked', 'closed']) {
    atForward = { error: reason };
    const result = await tryIt();
    assert.deepEqual(result.checks.at(-1), { name: 'address', origin: forward, extra: true, status: 'unreached', code: 'server_access_address_' + reason });
    assert.match(result.testId, /^[a-f0-9]{64}$/, reason);
  }
  // A fault: something answers wrongly there. No test to save with, as for a main address.
  for (const [answer, code, peer] of [[{ error: 'not_https' }, 'not_https'], [{ error: 'certificate' }, 'certificate'], [{ answer: null }, 'other_server'],
    [nonce => ({ answer: { protocol: accessProbeProtocol, nonce, admitted: false, reason: 'proxy_untrusted', peer: '172.18.0.1' } }), 'proxy_untrusted', '172.18.0.1'],
    [nonce => ({ answer: { protocol: accessProbeProtocol, nonce, admitted: false } }), 'not_admitted']]) {
    atForward = answer;
    const result = await tryIt();
    assert.deepEqual(result.checks.at(-1), { name: 'address', origin: forward, extra: true, ...(peer ? { peer } : {}), status: 'failed', code: 'server_access_address_' + code });
    assert.equal(result.testId, undefined, code); assert.equal(result.confirmation, undefined, code);
  }
  // Out of reach again: saved, and after the restart only the main address is asked.
  atForward = { error: 'no_answer' };
  await run.access.save({ config: next, testId: (await tryIt()).testId, baseRevision: base, confirm: true }, { ...run.context, fence: { token: randomBytes(32).toString('hex'), operationId: randomUUID() } });
  await run.access.task;
  assert.equal(run.exits, 1);
  const second = start(directory); await second.access.initialize(); await second.access.resume();
  assert.deepEqual(second.probes, [lan]); assert.equal(second.access.status().state, 'succeeded');
});

// The request 연결 테스트 really sends, at a listener of this test.
test('the request of 연결 테스트: one GET without a redirect, and why nothing came back', { timeout: 20000 }, async t => {
  const nonce = 'a'.repeat(64), seen = [];
  let reply = (req, res) => { res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ protocol: accessProbeProtocol, nonce, admitted: true })); };
  const other = createServer((req, res) => { seen.push({ method: req.method, url: req.url, host: req.headers.host, cookie: req.headers.cookie, authorization: req.headers.authorization }); reply(req, res); });
  await new Promise(resolve => other.listen(0, '127.0.0.1', resolve));
  t.after(() => { other.closeAllConnections(); other.close(); });
  const port = other.address().port, there = 'http://127.0.0.1:' + port;
  assert.deepEqual(await probeAddress(there, nonce), { answer: { protocol: accessProbeProtocol, nonce, admitted: true } });
  assert.deepEqual(seen, [{ method: 'GET', url: '/api/v1/access-probe/' + nonce, host: '127.0.0.1:' + port, cookie: undefined, authorization: undefined }]);
  reply = (req, res) => { res.writeHead(302, { Location: there + '/elsewhere' }); res.end(); };
  assert.deepEqual(await probeAddress(there, nonce), { answer: null }, 'a redirect is not followed');
  assert.equal(seen.length, 2);
  reply = (req, res) => { res.writeHead(200, { 'Content-Type': 'text/html' }); res.end('<html>router</html>'); };
  assert.deepEqual(await probeAddress(there, nonce), { answer: null }, 'another web page');
  reply = (req, res) => { res.writeHead(200); res.end('x'.repeat(8192)); };
  assert.deepEqual(await probeAddress(there, nonce), { answer: null }, 'a long answer is not read to its end');
  reply = () => {};
  assert.deepEqual(await probeAddress(there, nonce, { timeoutMs: 700 }), { error: 'no_answer' }, 'accepted, never answered');
  // TLS asked of a listener that speaks plain HTTP: an https address pointed straight at the server's port.
  assert.deepEqual(await probeAddress('https://127.0.0.1:' + port, nonce), { error: 'not_https' });
  const closed = await freePort();
  assert.deepEqual(await probeAddress('http://127.0.0.1:' + closed, nonce), { error: 'closed' });
  // What the system says of the name and the network, without asking either here.
  const failing = code => (options, callback) => { const request = httpRequest({ ...options, hostname: '127.0.0.1', port: closed }, callback); request.once('socket', socket => socket.destroy(Object.assign(Error(code), { code }))); return request; };
  for (const [code, reason] of [['ENOTFOUND', 'name_unknown'], ['EAI_AGAIN', 'unchecked'], ['ENETUNREACH', 'unchecked'], ['EHOSTUNREACH', 'closed'], ['ECONNRESET', 'closed'], ['CERT_HAS_EXPIRED', 'certificate'],
    ['DEPTH_ZERO_SELF_SIGNED_CERT', 'certificate'], ['ERR_TLS_CERT_ALTNAME_INVALID', 'certificate'], ['UNABLE_TO_VERIFY_LEAF_SIGNATURE', 'certificate'], ['ERR_SSL_WRONG_VERSION_NUMBER', 'not_https'], ['ETIMEDOUT', 'no_answer']])
    assert.deepEqual(await probeAddress('http://name.example:1', nonce, { http: failing(code) }), { error: reason }, code);
});

// The test's request to an address, sent to this test's listener instead: as
// itself, or as a reverse proxy on this machine would pass it on.
const loopbackProbe = (port, { proxy = null } = {}) => (origin, nonce) => {
  const ask = (options, callback) => httpRequest({ ...options, protocol: 'http:', hostname: '127.0.0.1', port, rejectUnauthorized: undefined,
    headers: { ...options.headers, ...(origin.startsWith('https://') && proxy ? proxy(new URL(origin)) : {}) } }, callback);
  return probeAddress(origin, nonce, { http: ask, https: ask });
};

test('the real server: 연결 테스트 asks this server at the address under test, which answers by the policy that would apply', { timeout: 30000 }, async t => {
  const directory = await volume(t), port = await freePort();
  let proxy = url => ({ 'X-Forwarded-Proto': 'https', 'X-Forwarded-Host': url.host });
  const f = await running(t, directory, port, { probeRequest: (origin, nonce) => loopbackProbe(port, { proxy })(origin, nonce) });
  const context = { managementOrigin: f.here }, runtime = f.server.accessRuntime, base = (await f.status()).revision;
  const named = 'http://vdi.example.com:' + port, checkOf = (result, origin) => result.checks.find(check => check.origin === origin);
  const get = (path, host) => new Promise((resolve, reject) => { const req = httpRequest({ hostname: '127.0.0.1', port, path, headers: { Host: host } }, res => { res.resume(); res.once('end', () => resolve(res.statusCode)); }); req.on('error', reject); req.end(); });

  // A new main address, not one the server lets in today: the test's own question is answered there, and only that.
  assert.equal(await get('/healthz', 'vdi.example.com:' + port), 403, 'not an address of this server yet');
  const moved = await runtime.test({ config: policy({ http: [named, f.here], primaryOrigin: named, listenPort: port }), baseRevision: base }, context);
  assert.deepEqual(checkOf(moved, named), { name: 'address', origin: named, status: 'passed', code: 'server_access_address_reached' });
  assert.match(moved.testId, /^[a-f0-9]{64}$/);
  assert.equal(await get('/healthz', 'vdi.example.com:' + port), 403, 'the test opened nothing');
  assert.equal(await get('/api/v1/access-probe/' + 'c'.repeat(64), 'vdi.example.com:' + port), 403, 'a value no test is waiting for is an ordinary refused request');
  assert.equal(await get('/api/v1/access-probe/' + 'c'.repeat(64), '192.0.2.10:' + port), 401, 'and at an address of this server, a path like any other that needs a login');

  // HTTPS behind a reverse proxy: reached when a trusted proxy passes the scheme and the name on.
  const https = trustedProxies => policy({ http: [f.here], https: [secure], primaryOrigin: secure, listenPort: port, trustedProxies });
  const behind = await runtime.test({ config: https(['127.0.0.1']), baseRevision: base }, context);
  assert.deepEqual(checkOf(behind, secure), { name: 'address', origin: secure, status: 'passed', code: 'server_access_address_reached' });
  // The proxy is not one this server would trust: said with where the request came from.
  const untrusted = await runtime.test({ config: https([]), baseRevision: base }, context);
  assert.deepEqual(checkOf(untrusted, secure), { name: 'address', origin: secure, peer: '127.0.0.1', status: 'failed', code: 'server_access_address_proxy_untrusted' });
  assert.equal(untrusted.testId, undefined); assert.equal(checkOf(untrusted, f.here).status, 'passed');
  // A trusted proxy that does not say the request was HTTPS, or passes another name on.
  proxy = () => ({});
  assert.equal(checkOf(await runtime.test({ config: https(['127.0.0.1']), baseRevision: base }, context), secure).code, 'server_access_address_proxy_headers');
  proxy = () => ({ 'X-Forwarded-Proto': 'https', 'X-Forwarded-Host': 'other.example.com' });
  assert.equal(checkOf(await runtime.test({ config: https(['127.0.0.1']), baseRevision: base }, context), secure).code, 'server_access_address_proxy_headers');
  // Nothing was saved, ended or restarted by any of it.
  assert.equal(f.run.exits, 0); assert.equal(runtime.blocked, false); assert.equal((await f.status()).revision, base); assert.equal((await f.status()).state, 'idle');
});

test('the request of 연결 테스트 over real TLS: a certificate nobody vouches for is not an answer', { timeout: 30000 }, async t => {
  if (spawnSync('openssl', ['version']).error?.code === 'ENOENT') { t.skip('OpenSSL unavailable'); return; }
  const directory = await volume(t), certPath = join(directory, 'cert.pem'), keyPath = join(directory, 'key.pem');
  assert.equal(spawnSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-keyout', keyPath, '-out', certPath, '-days', '1', '-subj', '/CN=localhost', '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1'], { encoding: 'utf8' }).status, 0);
  const nonce = 'b'.repeat(64);
  let asked = 0;
  const proxy = createTlsServer({ cert: await readFile(certPath), key: await readFile(keyPath) }, (req, res) => { asked++; res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ protocol: accessProbeProtocol, nonce, admitted: true })); });
  await new Promise(resolve => proxy.listen(0, '127.0.0.1', resolve));
  t.after(() => { proxy.closeAllConnections(); proxy.close(); });
  assert.deepEqual(await probeAddress('https://127.0.0.1:' + proxy.address().port, nonce), { error: 'certificate' });
  assert.equal(asked, 0, 'the request was never sent past the certificate');
});

// ---- an address this server cannot reach, proven by the administrator from outside ----
// A request as http.mjs would judge it: `origin` asked of this container's listener, an https one through a proxy on this machine.
const requestAt = origin => { const url = new URL(origin), secured = url.protocol === 'https:';
  return { rawHeaders: ['Host', url.host], headers: { host: url.host, ...(secured ? { 'x-forwarded-proto': 'https' } : {}) }, socket: { remoteAddress: '127.0.0.1', localPort: 18480, encrypted: false } }; };
const admittedAt = origin => config => { try { return resolveRequestAccess(requestAt(origin), config).origin; } catch { return null; } };
// One process whose 연결 테스트 cannot reach `unreached` from inside, with a clock the test moves.
function outsideRun(directory, { unreached = [secure], reason = 'no_answer', ...options } = {}) {
  // `end()`: the process is gone, and whatever it was waiting for is never continued.
  const clock = { now: Date.parse('2026-01-01T00:00:00Z') }, life = { over: false };
  const run = start(directory, { reach: origin => unreached.includes(origin) ? { error: reason } : true, now: () => clock.now, sleep: () => life.over ? new Promise(() => {}) : new Promise(resolve => setImmediate(resolve)), ...options });
  run.clock = clock; run.end = () => { life.over = true; };
  run.outside = async (config, context = run.context, more = {}) => {
    const base = run.access.status().revision, tested = await run.access.test({ config, baseRevision: base }, context);
    return run.access.saveOutside({ config, testId: tested.testId, baseRevision: base, confirm: true, outside: true }, context, { actor: 'owner', ...more });
  };
  return run;
}
const withHttps = policy({ http: [lan], https: [secure], primaryOrigin: secure });

test('a save with the administrator\'s own proof: nothing changes while it waits, the proof counts only from that address with the code, then it is the save any other is', async t => {
  const directory = await volume(t), run = outsideRun(directory);
  await run.access.initialize();
  const before = run.access.status().revision;
  const accepted = await run.outside(withHttps);
  assert.equal(accepted.state, 'awaiting_confirmation'); assert.match(accepted.operationId, /^[a-f0-9-]{36}$/);
  // Nothing was ended, fenced or written, and the policy in use is the one in use.
  assert.equal(run.exits, 0); assert.equal(run.ended, 0); assert.deepEqual(run.held, []);
  assert.deepEqual(await readdir(join(directory, 'access')), ['journal.json']); assert.equal((await journal(directory)).state, 'awaiting_confirmation');
  const watching = run.access.status({ managementOrigin: lan });
  assert.equal(watching.state, 'awaiting_confirmation'); assert.equal(watching.revision, before); assert.equal(watching.pendingConfig.https.origin, secure);
  assert.deepEqual(watching.outside.origins, [{ origin: secure, confirmed: false }]); assert.equal(watching.outside.remainingMs, 300000); assert.equal(watching.outside.windowMs, 300000);
  assert.match(watching.outside.code, /^\d{6}$/);
  // The code is given where the save can be watched, never at the address it has to be brought to, nor to a status nobody asked.
  assert.equal(run.access.status({ managementOrigin: secure }).outside.code, undefined); assert.equal(run.access.status().outside.code, undefined);
  // The address under proof leads to the console meanwhile, by the policy being saved; nothing else does.
  assert.deepEqual(run.access.provisionalAccess(requestAt(secure)), { origin: secure, bootstrapOnly: false, proxied: true, provisional: true });
  assert.equal(run.access.provisionalAccess(requestAt('https://other.example.com')), null);
  assert.equal(run.access.provisionalAccess(requestAt(lan)), null, 'an address of the policy in use needs no such door');
  assert.equal(run.access.provisionalAccess({ ...requestAt(secure), headers: { host: 'vdi.example.com' } }), null, 'not as plain HTTP: the policy being saved would not let that in either');
  assert.equal(run.access.provisionalAccess({ ...requestAt(secure), socket: { remoteAddress: '203.0.113.9', localPort: 18480, encrypted: false } }), null, 'nor from a proxy it does not trust');
  // Another test or save waits its turn, and the command line sees a save in flight.
  await assert.rejects(run.access.test({ config: withHttps, baseRevision: before }, run.context), refused(409, 'server_access_busy'));
  const code = watching.outside.code, operationId = accepted.operationId, wrong = code === '000000' ? '000001' : '000000';
  const prove = (input, origin, context = { managementOrigin: origin }) => run.access.confirmOutside(input, context, { actor: 'owner', admitted: admittedAt(origin) });
  // What does not count: the old address, the right address without the code, another save, a made-up shape.
  await assert.rejects(prove({ operationId, code }, lan), refused(409, 'server_access_confirmation_origin'));
  await assert.rejects(prove({ operationId, code }, secure, { managementOrigin: lan }), refused(409, 'server_access_confirmation_origin'));
  await assert.rejects(run.access.confirmOutside({ operationId, code }, { managementOrigin: secure }, { actor: 'owner' }), refused(409, 'server_access_confirmation_origin'), 'an address only claimed, not one the request was let in at');
  await assert.rejects(prove({ operationId, code: wrong }, secure), refused(409, 'server_access_confirmation_code'));
  await assert.rejects(prove({ operationId: randomUUID(), code }, secure), refused(409, 'server_access_confirmation_unavailable'));
  for (const input of [{ operationId }, { operationId, code: '12345' }, { operationId, code: 123456 }, { operationId, code, extra: true }, { code }]) await assert.rejects(prove(input, secure), refused(400, 'server_access_command_invalid'));
  assert.deepEqual(run.access.status({ managementOrigin: lan }).outside.origins, [{ origin: secure, confirmed: false }]); assert.equal(run.exits, 0);
  // The proof.
  run.clock.now += 120000;
  assert.equal(run.access.status({ managementOrigin: lan }).outside.remainingMs, 180000);
  assert.deepEqual(await prove({ operationId, code }, secure), { accepted: true, operationId, origin: secure, remaining: [] });
  await run.access.task;
  assert.equal(run.exits, 1, 'the restart of a save');
  assert.equal(run.held.length, 1); assert.equal(run.held[0].operationId, operationId); assert.equal(run.held[0].actor, 'owner'); assert.equal(run.held[0].event, 'server_access_connections_ended');
  const written = await journal(directory);
  assert.equal(written.state, 'applying'); assert.deepEqual(written.outside.confirmed, [secure]); assert.equal(written.outside.code, null, 'the code is gone with the window');
  assert.equal(JSON.parse(await readFile(join(directory, 'access', 'policy.json'), 'utf8')).config.https.origin, secure);
  assert.equal(run.access.status({ managementOrigin: lan }).outside, undefined);
  assert.deepEqual(run.records, [['server_access_outside_started', 'owner', operationId + ':' + secure], ['server_access_outside_confirmed', 'owner', operationId + ':' + secure]]);
  await assert.rejects(prove({ operationId, code }, secure), refused(409, 'server_access_confirmation_unavailable'));
  // The restarted server asks itself at the address it can reach, and takes the proven one as proven.
  const second = start(directory, { healthy: origin => origin !== secure });
  await second.access.initialize(); await second.access.resume();
  assert.deepEqual(second.probes, [lan]); assert.equal(second.exits, 0);
  assert.equal(second.access.status().state, 'succeeded'); assert.equal(second.config.accessConfig.primaryOrigin, secure);
  assert.equal(second.access.provisionalAccess(requestAt(secure)), null);
});

test('a proof that does not come in time, and a save withdrawn while it waits: nothing changed, nobody was disconnected, and the door is shut again', async t => {
  const directory = await volume(t), run = outsideRun(directory);
  await run.access.initialize();
  const before = run.access.status().revision, accepted = await run.outside(withHttps), code = run.access.status({ managementOrigin: lan }).outside.code;
  run.clock.now += 299999; await new Promise(resolve => setImmediate(resolve));
  assert.equal(run.access.status().state, 'awaiting_confirmation'); assert.ok(run.access.provisionalAccess(requestAt(secure)));
  run.clock.now += 1;
  assert.equal(run.access.provisionalAccess(requestAt(secure)), null, 'shut at the minute, not at the next look');
  await assert.rejects(run.access.confirmOutside({ operationId: accepted.operationId, code }, { managementOrigin: secure }, { admitted: admittedAt(secure) }), refused(409, 'server_access_confirmation_unavailable'));
  await run.access.task;
  const ended = run.access.status({ managementOrigin: lan });
  assert.equal(ended.state, 'failed'); assert.equal(ended.errorCode, 'server_access_confirmation_timeout'); assert.equal(ended.failedOrigin, secure, 'the address that was not proven is named');
  assert.equal(ended.unchangedVerified, true); assert.equal(ended.revision, before); assert.equal(ended.pendingConfig, null); assert.equal(ended.outside, undefined);
  assert.equal(run.exits, 0); assert.equal(run.ended, 0); assert.deepEqual(run.held, []);
  assert.deepEqual(await readdir(join(directory, 'access')), ['journal.json'], 'no policy was written');
  assert.deepEqual(run.records.map(row => row[0]), ['server_access_outside_started', 'server_access_outside_expired']); assert.equal(run.records[1][2], accepted.operationId + ':' + secure);
  // The form is free again: the same change, withdrawn this time.
  const again = await run.outside(withHttps);
  assert.equal(run.access.awaiting(again.operationId), true);
  const withdrawn = await run.access.cancel({ operationId: again.operationId });
  assert.equal(withdrawn.cancelled, true);
  const cancelled = run.access.status({ managementOrigin: lan });
  assert.equal(cancelled.state, 'failed'); assert.equal(cancelled.errorCode, 'server_access_cancelled'); assert.equal(cancelled.unchangedVerified, true); assert.equal(cancelled.failedOrigin, undefined);
  assert.equal(run.access.provisionalAccess(requestAt(secure)), null); assert.equal(run.access.awaiting(again.operationId), false);
  assert.equal(run.exits, 0); assert.deepEqual(run.held, []);
});

test('a restart of the server while a save waits for its proof, or after the proof and before anything was written: nothing changed and it says so', async t => {
  const directory = await volume(t), first = outsideRun(directory);
  await first.access.initialize();
  const before = first.access.status().revision, accepted = await first.outside(withHttps);
  // The process ends here. The next one finds the save still waiting.
  first.end();
  const second = outsideRun(directory);
  await second.access.initialize();
  const interrupted = second.access.status({ managementOrigin: lan });
  assert.equal(interrupted.state, 'failed'); assert.equal(interrupted.errorCode, 'server_access_interrupted'); assert.equal(interrupted.unchangedVerified, true);
  assert.equal(interrupted.operationId, accepted.operationId); assert.equal(interrupted.revision, before); assert.equal(interrupted.outside, undefined);
  assert.equal(second.access.startupOperationId, null); assert.equal(second.access.provisionalAccess(requestAt(secure)), null);
  assert.equal(second.config.accessConfig.https.enabled, false);
  // Proven, and then held by the server's own unfinished work: a restart there has written nothing either.
  const third = outsideRun(directory, { activity: () => { const report = quiet(); report.activity.operations.hostUpdates = 1; return report; } });
  await third.access.initialize();
  const waiting = await third.outside(withHttps), code = third.access.status({ managementOrigin: lan }).outside.code;
  await third.access.confirmOutside({ operationId: waiting.operationId, code }, { managementOrigin: secure }, { actor: 'owner', admitted: admittedAt(secure) });
  await eventually(() => third.access.status().state === 'waiting_idle' && third.access.status().blockers?.length === 1);
  assert.deepEqual(third.access.status().blockers, [{ code: 'server_access_host_updates_busy', count: 1 }]); assert.equal(third.held.length, 1, 'behind the fence from the proof on');
  assert.ok(third.access.provisionalAccess(requestAt(secure)), 'the proven address keeps its console until the save ends');
  third.end();
  const fourth = outsideRun(directory);
  await fourth.access.initialize();
  assert.equal(fourth.access.status().state, 'failed'); assert.equal(fourth.access.status().errorCode, 'server_access_interrupted'); assert.equal(fourth.access.status().unchangedVerified, true);
  assert.deepEqual(await readdir(join(directory, 'access')), ['journal.json']);
});

test('a save made at the very address under proof needs no second proof; with two addresses under proof each is proven on its own', async t => {
  const directory = await volume(t), prepared = start(directory);
  await prepared.access.initialize(); await prepared.change(withHttps);
  const installed = start(directory); await installed.access.initialize(); await installed.access.resume();
  assert.equal(installed.access.status().state, 'succeeded');
  // The administrator works at the HTTPS address, which this server cannot reach from inside, and adds an address.
  const run = outsideRun(directory);
  await run.access.initialize();
  const more = policy({ http: [lan, extra], https: [secure], primaryOrigin: secure });
  const accepted = await run.outside(more, { managementOrigin: secure }, { admitted: admittedAt(secure) });
  await run.access.task;
  assert.equal(accepted.state, 'awaiting_confirmation'); assert.equal(run.exits, 1, 'the request that saved it was the proof');
  assert.deepEqual((await journal(directory)).outside.confirmed, [secure]);
  assert.deepEqual(run.records.map(row => row[0]), ['server_access_outside_started']);
  // The same request without the proof that it was let in there is no proof.
  const claimed = outsideRun(directory); await claimed.access.initialize(); await claimed.access.resume();
  const still = policy({ http: [lan], https: [secure], primaryOrigin: secure });
  await claimed.outside(still, { managementOrigin: secure });
  assert.equal(claimed.access.status().state, 'awaiting_confirmation'); assert.deepEqual(claimed.access.status().outside.origins, [{ origin: secure, confirmed: false }]);
  await claimed.access.cancel({ operationId: claimed.access.status().operationId });
  // Both main addresses out of reach from inside: one proof each, and the save goes on with the second.
  const outsideHttp = 'http://vdi.example.com:18480', both = policy({ http: [outsideHttp, lan], https: [secure], primaryOrigin: secure });
  const two = outsideRun(directory, { unreached: [secure, outsideHttp] }); await two.access.initialize();
  const pending = await two.outside(both, { managementOrigin: lan }), code = two.access.status({ managementOrigin: lan }).outside.code;
  const first = await two.access.confirmOutside({ operationId: pending.operationId, code }, { managementOrigin: outsideHttp }, { actor: 'owner', admitted: admittedAt(outsideHttp) });
  assert.deepEqual(first.remaining, [secure]);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(two.exits, 0); assert.equal(two.access.status().state, 'awaiting_confirmation');
  assert.deepEqual(two.access.status({ managementOrigin: lan }).outside.origins, [{ origin: secure, confirmed: false }, { origin: outsideHttp, confirmed: true }]);
  assert.match(two.access.status({ managementOrigin: outsideHttp }).outside.code, /^\d{6}$/, 'a proven address may show the code for the other');
  await two.access.confirmOutside({ operationId: pending.operationId, code }, { managementOrigin: secure }, { actor: 'owner', admitted: admittedAt(secure) });
  await two.access.task;
  assert.equal(two.exits, 1);
});

test('the proof from outside is not offered for a real fault, nor when a real fault sits beside an address that is merely out of reach', async t => {
  const directory = await volume(t);
  for (const reason of ['certificate', 'not_https']) {
    const run = outsideRun(directory, { reason }); await run.access.initialize();
    const base = run.access.status().revision, tested = await run.access.test({ config: withHttps, baseRevision: base }, run.context);
    assert.equal(tested.confirmation, undefined, reason); assert.equal(tested.testId, undefined, reason);
    await assert.rejects(run.access.saveOutside({ config: withHttps, testId: 'e'.repeat(64), baseRevision: base, confirm: true, outside: true }, run.context), refused(409, 'server_access_test_required'));
  }
  const mixed = start(directory, { reach: origin => origin === secure ? { answer: null } : { error: 'no_answer' } }); await mixed.access.initialize();
  const tested = await mixed.access.test({ config: withHttps, baseRevision: mixed.access.status().revision }, mixed.context);
  assert.deepEqual(tested.checks.filter(check => check.name === 'address').map(check => check.code), ['server_access_address_other_server', 'server_access_address_no_answer']);
  assert.equal(tested.confirmation, undefined); assert.equal(tested.testId, undefined);
  // A test every address of which was reached is saved the ordinary way, not this one.
  const reached = start(directory); await reached.access.initialize();
  const base = reached.access.status().revision, passed = await reached.access.test({ config: withHttps, baseRevision: base }, reached.context);
  await assert.rejects(reached.access.saveOutside({ config: withHttps, testId: passed.testId, baseRevision: base, confirm: true, outside: true }, reached.context), refused(409, 'server_access_test_required'));
  await assert.rejects(reached.access.saveOutside({ config: withHttps, testId: passed.testId, baseRevision: base, confirm: true }, reached.context), refused(400, 'server_access_confirmation_required'));
});
