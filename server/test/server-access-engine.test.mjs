import test from 'node:test';
import assert from 'node:assert/strict';
import { ServerAccessEngine, serverAccessQuiescence } from '../update/server-access-engine.mjs';
import { normalizeServerAccess, serverAccessHash } from '../update/server-access-policy.mjs';

const token = 'e'.repeat(64), operationId = '12345678-1234-4234-8234-123456789abc';
const config = () => normalizeServerAccess({ protocol: 'server-access-v1', primaryOrigin: 'https://alpha.example', https: { enabled: true, origin: 'https://alpha.example', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18480 }, http: { enabled: false, origin: null, listenHost: '127.0.0.1', listenPort: 18481 }, trustedProxies: ['127.0.0.1'] });
const idle = () => ({ protocol: 'activity-v1', sessions: { total: 0, stream: 0, attached: 0, tickets: 0 }, channels: { clipboard: 0, clipboardTransfers: 0, drive: 0, driveShares: 0, driveRequests: 0, usb: 0, usbDevices: 0, print: 0, printTransfers: 0 }, operations: { display: 0, sessionCleanup: 0, hostUpdates: 0, power: 0 }, hosts: { connected: 4 }, live: { channels: 6 }, sinceActivityMs: 70000, uptimeMs: 100000 });
async function fixture({ saved, verified = true } = {}) {
  let applied = config(), clock = 100000, journal = saved ?? null;
  const calls = [], controls = { verified, marker: true, health: true, activity: idle(), consoles: 0, failPreflight: false };
  // The fenced report as the broker sends it: activity-v1 plus who is connected.
  const report = () => ({ protocol: 'server-access-activity-v1', fence: true, activity: structuredClone(controls.activity), connections: { people: controls.activity.sessions.total + controls.consoles, sessions: controls.activity.sessions.total, consoles: controls.consoles } });
  const current = () => ({ config: applied, revision: serverAccessHash(applied), installationId: 'a'.repeat(64), managed: true, applySupported: true, deploymentKind: 'systemd', legacy: false, dockerPorts: [], envSha256: 'env', accessSha256: serverAccessHash(applied) });
  const adapter = {
    initialize: async () => {}, current: async () => current(),
    preflight: async () => { if (controls.failPreflight) throw Error('server_access_tls_identity_invalid'); return { passed: true, checks: [{ name: 'route', status: 'pending', code: 'server_access_pending_until_apply' }] }; },
    activity: async (_current, secret) => { calls.push('activity'); assert.equal(secret, token); return report(); },
    // The broker ends its own sessions, their side channels and open Consoles.
    endConnections: async (_current, secret) => {
      calls.push('end'); assert.equal(secret, token); if (controls.endGate) await controls.endGate;
      const ended = { people: controls.activity.sessions.total + controls.consoles, sessions: controls.activity.sessions.total, consoles: controls.consoles };
      controls.activity.sessions = { total: 0, stream: 0, attached: 0, tickets: 0 }; for (const key of Object.keys(controls.activity.channels)) controls.activity.channels[key] = 0;
      controls.activity.operations.display -= controls.consoles; controls.consoles = 0;
      return { protocol: 'server-access-connections-v1', fence: true, ended };
    },
    prepare: async (next, { previous }) => ({ config: next, revision: serverAccessHash(next), previous }),
    apply: async job => { calls.push('apply'); applied = job.config; },
    verify: async () => { calls.push('verify'); if (controls.gate) await controls.gate; return controls.health; },
    rollback: async job => { calls.push('rollback'); applied = job.previous.config; return controls.verified; },
    unchanged: async () => controls.verified,
    unchangedAfterRestart: async previous => { calls.push('unchanged-health'); return controls.verified && current().revision === previous.revision; },
    clearAdmissionMarker: async () => controls.marker,
  };
  const engine = new ServerAccessEngine({ adapter, storage: { initialize: async () => {}, read: async () => journal, write: async value => { journal = structuredClone(value); } }, now: () => clock, sleep: async ms => { clock += ms; controls.onSleep?.(); if (controls.sleepGate) await controls.sleepGate; }, pollMs: 1, maxWaitMs: 6 });
  await engine.initialize();
  return { engine, adapter, controls, calls, advance: ms => { clock += ms; }, context: { managementOrigin: applied.primaryOrigin, fence: { token, operationId } }, candidate: { ...applied, https: { ...applied.https, listenPort: 19000 } } };
}
// Saving ends the connections, the server comes
// first. Only the broker's own unfinished work may hold a save back.
const connected = (value, consoles = 0) => ({ protocol: 'server-access-activity-v1', fence: true, activity: value, connections: { people: value.sessions.total + consoles, sessions: value.sessions.total, consoles } });
const withSessions = (value, count) => { value.sessions = { total: count, stream: count, attached: count, tickets: 0 }; return value; };
test('only the broker\'s own unfinished work blocks; connections are open work to end; recent activity and Host tidy-up hold nothing', () => {
  assert.deepEqual(serverAccessQuiescence(connected(idle())), { blockers: [], open: false });
  for (const quiet of [v => { v.sinceActivityMs = 1; }, v => { v.operations.sessionCleanup = 2; }, v => { v.live.channels = 40; }]) { const value = idle(); quiet(value); assert.deepEqual(serverAccessQuiescence(connected(value)), { blockers: [], open: false }); }
  for (const open of [v => withSessions(v, 3), v => { v.channels.driveShares = 1; }, v => { v.channels.clipboardTransfers = 1; }]) { const value = idle(); open(value); assert.deepEqual(serverAccessQuiescence(connected(value)), { blockers: [], open: true }); }
  const viewer = idle(); viewer.operations.display = 1;
  assert.deepEqual(serverAccessQuiescence(connected(viewer, 1)), { blockers: [], open: true }, 'an open Console is a connection, not a display change');
  const busy = withSessions(idle(), 2); busy.operations = { display: 3, sessionCleanup: 0, hostUpdates: 3, power: 1 };
  assert.deepEqual(serverAccessQuiescence(connected(busy, 1)), { open: true, blockers: [{ code: 'server_access_host_updates_busy', count: 3 }, { code: 'server_access_desktop_settings_busy', count: 2 }, { code: 'server_access_power_busy', count: 1 }] });
  const unverified = { blockers: [{ code: 'server_access_activity_unverified', count: 0 }], open: false };
  for (const broken of [v => { delete v.activity.operations; }, v => { delete v.connections; }, v => { v.connections.sessions = 1; }, v => { v.connections.consoles = 1; }, v => { v.activity.operations.power = null; }, v => { v.connections.extra = 0; }]) { const value = connected(idle()); broken(value); assert.deepEqual(serverAccessQuiescence(value), unverified); }
});
const saved = async (f, id = operationId) => {
  const baseRevision = (await f.engine.status()).revision, checked = await f.engine.test({ config: f.candidate, baseRevision }, { managementOrigin: f.context.managementOrigin });
  return f.engine.save({ config: f.candidate, baseRevision, testId: checked.testId, confirm: true }, { ...f.context, fence: { token, operationId: id } });
};
const until = async predicate => { for (let attempt = 0; attempt < 2000 && !predicate(); attempt++) await Promise.resolve(); assert.equal(predicate(), true, 'the engine reached the expected point'); };
test('people are connected: the broker ends the connections, then the change is applied at once', async () => {
  const f = await fixture(); withSessions(f.controls.activity, 2); f.controls.activity.channels.clipboard = 2; f.controls.activity.operations.display = 1; f.controls.consoles = 1; f.controls.activity.sinceActivityMs = 5;
  await saved(f); await f.engine.task;
  const done = await f.engine.status(); assert.equal(done.state, 'succeeded'); assert.equal(done.config.https.listenPort, 19000); assert.equal(done.pendingConfig, null);
  assert.equal(f.calls.filter(call => call === 'end').length, 1); assert.ok(f.calls.indexOf('end') < f.calls.indexOf('apply'), 'connections end before the first write');
  assert.equal(done.blockers?.length ?? 0, 0); assert.equal(done.waitUntil, undefined);
});
test('nobody is connected: the change is applied without ending anything, even right after a connection ended', async () => {
  const f = await fixture(); f.controls.activity.sinceActivityMs = 1; f.controls.activity.operations.sessionCleanup = 1;
  await saved(f); await f.engine.task;
  assert.equal((await f.engine.status()).state, 'succeeded'); assert.equal(f.calls.includes('end'), false); assert.equal(f.calls.includes('apply'), true);
});
test('a running Host update holds it: the reason and the bound are reported, nobody is disconnected, and a cancel restores the applied state', async () => {
  const f = await fixture(), base = await f.engine.status(); let release;
  withSessions(f.controls.activity, 1); f.controls.activity.operations.hostUpdates = 3; f.controls.sleepGate = new Promise(resolve => { release = resolve; });
  await saved(f); await until(() => f.engine.state.blockers?.length === 1);
  const waiting = await f.engine.status();
  assert.equal(waiting.state, 'waiting_idle'); assert.deepEqual(waiting.blockers, [{ code: 'server_access_host_updates_busy', count: 3 }]); assert.deepEqual(waiting.idleReasons, ['server_access_host_updates_busy']);
  assert.equal(waiting.waitUntil, new Date(100000 + 6).toISOString(), 'the wait is bounded and its end is visible'); assert.equal(waiting.pendingConfig.https.listenPort, 19000);
  await assert.rejects(f.engine.cancel({ operationId: '22345678-1234-4234-8234-123456789abc' }), /cancel_unavailable/);
  await assert.rejects(f.engine.cancel({ operationId, extra: true }), /command_invalid/);
  const cancelled = await f.engine.cancel({ operationId });
  assert.deepEqual(cancelled, { accepted: true, operationId, state: 'failed', cancelled: true });
  const status = await f.engine.status();
  assert.equal(status.state, 'failed'); assert.equal(status.errorCode, 'server_access_cancelled'); assert.equal(status.unchangedVerified, true, 'the verified unchanged result is what releases the broker fence');
  assert.equal(status.pendingConfig, null); assert.deepEqual(status.config, base.config); assert.equal(status.revision, base.revision); assert.deepEqual(status.blockers, []); assert.equal(status.waitUntil, undefined);
  assert.equal(f.calls.includes('end'), false, 'nobody was disconnected while it waited'); assert.equal(f.calls.includes('apply'), false); assert.equal(f.engine.busy, false);
  await assert.rejects(f.engine.cancel({ operationId }), /cancel_unavailable/);
  // The Host update is over: the same change, saved again, ends the connection and applies.
  release(); f.controls.sleepGate = null; f.controls.activity.operations.hostUpdates = 0;
  await saved(f, '32345678-1234-4234-8234-123456789abc'); await f.engine.task;
  assert.equal((await f.engine.status()).state, 'succeeded'); assert.deepEqual(f.calls.filter(call => call === 'end' || call === 'apply'), ['end', 'apply']);
});
test('the broker\'s work ends while it waits: the connections are ended only then, and the change is applied', async () => {
  const f = await fixture(); let sleeps = 0;
  withSessions(f.controls.activity, 2); f.controls.activity.operations.power = 1;
  f.controls.onSleep = () => { if (++sleeps === 3) { assert.equal(f.calls.includes('end'), false); f.controls.activity.operations.power = 0; } };
  await saved(f); await f.engine.task;
  const done = await f.engine.status(); assert.equal(done.state, 'succeeded'); assert.deepEqual(done.blockers, []);
  assert.deepEqual(f.calls.filter(call => call === 'end' || call === 'apply'), ['end', 'apply']);
});
test('a wait that outlasts its bound fails unchanged and names what blocked it', async () => {
  const f = await fixture(), base = await f.engine.status();
  withSessions(f.controls.activity, 1); f.controls.activity.operations.hostUpdates = 3; f.controls.activity.operations.display = 1;
  await saved(f); await f.engine.task;
  const status = await f.engine.status();
  assert.equal(status.state, 'failed'); assert.equal(status.errorCode, 'server_access_idle_timeout'); assert.equal(status.unchangedVerified, true);
  assert.deepEqual(status.blockers, [{ code: 'server_access_host_updates_busy', count: 3 }, { code: 'server_access_desktop_settings_busy', count: 1 }]);
  assert.equal(status.pendingConfig, null); assert.deepEqual(status.config, base.config);
  assert.equal(f.calls.includes('end'), false, 'a timeout disconnects nobody'); assert.equal(f.calls.includes('apply'), false);
});
test('once the connections are being ended a cancel is refused and the apply goes on', async () => {
  const f = await fixture(); let release;
  withSessions(f.controls.activity, 1); f.controls.endGate = new Promise(resolve => { release = resolve; });
  await saved(f); await until(() => f.calls.includes('end'));
  assert.equal(f.engine.state.state, 'waiting_idle');
  await assert.rejects(f.engine.cancel({ operationId }), /cancel_too_late/);
  release(); await f.engine.task;
  assert.equal((await f.engine.status()).state, 'succeeded');
});
test('connections that do not end within their bound fail unchanged; a lost fence while ending does too', async () => {
  const stuck = await fixture(); withSessions(stuck.controls.activity, 1);
  stuck.adapter.endConnections = async () => { stuck.calls.push('end'); return { protocol: 'server-access-connections-v1', fence: true, ended: { people: 0, sessions: 0, consoles: 0 } }; };
  stuck.engine.endWaitMs = 3; await saved(stuck); await stuck.engine.task;
  let status = await stuck.engine.status(); assert.equal(status.state, 'failed'); assert.equal(status.errorCode, 'server_access_connections_busy'); assert.equal(status.unchangedVerified, true); assert.equal(stuck.calls.includes('apply'), false);
  const lost = await fixture(); withSessions(lost.controls.activity, 1); lost.adapter.endConnections = async () => null;
  await saved(lost); await lost.engine.task;
  status = await lost.engine.status(); assert.equal(status.state, 'failed'); assert.equal(status.errorCode, 'server_access_fence_lost'); assert.equal(lost.calls.includes('apply'), false);
});
test('a failed health check after the connections were ended is still a verified rollback', async () => {
  const f = await fixture(), base = await f.engine.status(); withSessions(f.controls.activity, 2); f.controls.health = false;
  await saved(f); await f.engine.task;
  const status = await f.engine.status();
  assert.equal(status.state, 'rolled_back'); assert.equal(status.rollbackVerified, true); assert.equal(status.errorCode, 'server_access_health_failed');
  assert.deepEqual(status.config, base.config); assert.equal(status.pendingConfig, null);
  assert.deepEqual(f.calls.filter(call => ['end', 'apply', 'verify', 'rollback'].includes(call)), ['end', 'apply', 'verify', 'rollback']);
});
test('a preflight token cannot authorize edited, expired or unconfirmed config', async () => {
  const f = await fixture(), baseRevision = (await f.engine.status()).revision;
  const checked = await f.engine.test({ config: f.candidate, baseRevision }, { managementOrigin: f.context.managementOrigin });
  await assert.rejects(f.engine.save({ config: { ...f.candidate, trustedProxies: [] }, baseRevision, testId: checked.testId, confirm: true }, f.context), /test_required/);
  await assert.rejects(f.engine.save({ config: f.candidate, baseRevision, testId: checked.testId, confirm: false }, f.context), /confirmation_required/);
  f.advance(300001);
  await assert.rejects(f.engine.save({ config: f.candidate, baseRevision, testId: checked.testId, confirm: true }, f.context), /test_required/);
  assert.equal(f.calls.includes('apply'), false);
});
test('applied config stays old until actual health gate; provided op survives and nonce never enters status', async () => {
  const f = await fixture(), base = await f.engine.status(); let release;
  f.controls.gate = new Promise(resolve => { release = resolve; });
  const checked = await f.engine.test({ config: f.candidate, baseRevision: base.revision }, { managementOrigin: f.context.managementOrigin });
  const accepted = await f.engine.save({ config: f.candidate, baseRevision: base.revision, testId: checked.testId, confirm: true }, f.context);
  assert.equal(accepted.operationId, operationId);
  for (let attempt = 0; attempt < 1000 && !f.calls.includes('verify'); attempt++) await Promise.resolve();
  assert.equal(f.calls.includes('verify'), true, 'the controlled actual-health gate was reached');
  const pending = await f.engine.status(); assert.deepEqual(pending.config, base.config); assert.equal(pending.pendingConfig.https.listenPort, 19000); assert.equal(pending.state, 'health_check'); assert.equal(JSON.stringify(pending).includes(token), false);
  release(); await f.engine.task;
  const done = await f.engine.status(); assert.equal(done.state, 'succeeded'); assert.equal(done.config.https.listenPort, 19000); assert.equal(done.pendingConfig, null); assert.equal(done.operationId, operationId);
});
test('failed actual route is verified rollback, and marker cleanup never rolls back already healthy settings', async () => {
  for (const failRoute of [true, false]) {
    const f = await fixture(), baseRevision = (await f.engine.status()).revision; f.controls.health = !failRoute; f.controls.marker = false;
    const checked = await f.engine.test({ config: f.candidate, baseRevision }, { managementOrigin: f.context.managementOrigin });
    await f.engine.save({ config: f.candidate, baseRevision, testId: checked.testId, confirm: true }, f.context); await f.engine.task;
    const status = await f.engine.status(); assert.equal(status.state, failRoute ? 'rolled_back' : 'succeeded'); assert.equal(status.rollbackVerified, failRoute); assert.equal(status.admissionMarkerCleared, false); assert.equal(f.calls.includes('rollback'), failRoute);
  }
});
test('interrupted waiting recovers only with current revision plus live health, without an old fence nonce', async () => {
  const original = config(), saved = { state: 'waiting_idle', appliedConfig: original, appliedRevision: serverAccessHash(original), pendingConfig: original, job: null, operationId };
  for (const verified of [true, false]) {
    const f = await fixture({ saved, verified }), status = await f.engine.status();
    assert.equal(status.state, verified ? 'failed' : 'recovery_required'); assert.equal(status.unchangedVerified, verified); assert.equal(f.calls.includes('activity'), false); assert.equal(f.calls.includes('apply'), false);
  }
});
test('failed read-only test retains the previous terminal operation for ack-loss reconciliation', async () => {
  const original = config(), saved = { state: 'succeeded', appliedConfig: original, appliedRevision: serverAccessHash(original), pendingConfig: null, operationId, rollbackVerified: false };
  const f = await fixture({ saved }); f.controls.failPreflight = true;
  await assert.rejects(f.engine.test({ config: f.candidate, baseRevision: serverAccessHash(original) }, { managementOrigin: original.primaryOrigin }), /tls_identity_invalid/);
  const status = await f.engine.status(); assert.equal(status.operationId, operationId); assert.equal(status.state, 'succeeded'); assert.equal(status.pendingConfig, null);
});

test('a terminal success cannot present an old dual policy as applied after actual storage revision changed', async () => {
  const original = config(), previous = normalizeServerAccess({ ...original, http: { ...original.http, enabled: true, origin: 'http://alpha.example:8080' } });
  const saved = { state: 'succeeded', appliedConfig: previous, appliedRevision: serverAccessHash(previous), appliedLegacy: false, appliedInstallationId: 'a'.repeat(64), pendingConfig: null, operationId };
  const f = await fixture({ saved }), status = await f.engine.status();
  assert.equal(status.state, 'recovery_required'); assert.equal(status.scope, 'unverified'); assert.equal(status.applySupported, false);
  assert.equal(status.currentObserved, true); assert.equal(status.revision, serverAccessHash(original)); assert.equal(status.config.http.enabled, false);
  assert.equal(status.lastKnownRevision, serverAccessHash(previous)); assert.equal(status.lastKnownConfig.http.enabled, true);
  assert.deepEqual(f.calls, [], 'status mismatch never reapplies settings or rolls back an old completed job');
});

test('same-revision legacy fallback is still detected and cannot revive a preflight token', async () => {
  const original = config(), saved = { state: 'succeeded', appliedConfig: original, appliedRevision: serverAccessHash(original), pendingConfig: null, operationId };
  const f = await fixture({ saved }), current = f.adapter.current;
  const checked = await f.engine.test({ config: f.candidate, baseRevision: serverAccessHash(original) }, { managementOrigin: original.primaryOrigin });
  f.adapter.current = async () => ({ ...await current(), legacy: true, accessSha256: null });
  await assert.rejects(f.engine.save({ config: f.candidate, baseRevision: serverAccessHash(original), testId: checked.testId, confirm: true }, { ...f.context, fence: { token, operationId: '22345678-1234-4234-8234-123456789abc' } }), /recovery_required/);
  const status = await f.engine.status(); assert.equal(status.state, 'recovery_required'); assert.equal(status.scope, 'unverified'); assert.equal(status.legacy, true);
  assert.equal(status.currentRevision, status.lastKnownRevision, 'matching hash alone does not make legacy storage equivalent');
  assert.equal(f.engine.tests.size, 0); assert.equal(f.calls.includes('apply'), false);
});

test('a replacement installation identity is refused before preflight even when policy bytes and revision match', async () => {
  const f = await fixture(), current = f.adapter.current; f.adapter.current = async () => ({ ...await current(), installationId: 'b'.repeat(64) });
  await assert.rejects(f.engine.test({ config: f.candidate, baseRevision: serverAccessHash(config()) }, { managementOrigin: config().primaryOrigin }), /recovery_required/);
  const status = await f.engine.status(); assert.equal(status.state, 'recovery_required'); assert.equal(status.applySupported, false); assert.equal(f.calls.includes('apply'), false);
});

test('unreadable current storage clears applied claims instead of publishing the cached terminal success', async () => {
  const original = config(), saved = { state: 'succeeded', appliedConfig: original, appliedRevision: serverAccessHash(original), appliedLegacy: false, appliedInstallationId: 'a'.repeat(64), pendingConfig: null, operationId };
  const f = await fixture({ saved }); f.adapter.current = async () => { throw Error('server_access_state_missing'); };
  const status = await f.engine.status();
  assert.equal(status.state, 'recovery_required'); assert.equal(status.scope, 'unverified'); assert.equal(status.currentObserved, false);
  assert.equal(status.config, null); assert.equal(status.currentRevision, null); assert.equal(status.revision, serverAccessHash(original), 'legacy UI revision remains an explicitly unverified last-known token');
  assert.deepEqual(status.lastKnownConfig, original); assert.equal(status.lastKnownRevision, serverAccessHash(original));
  assert.equal(status.applySupported, false); assert.equal(status.rollbackVerified, false); assert.equal(status.unchangedVerified, false);
  assert.equal(status.errorCode, 'server_access_state_missing'); assert.deepEqual(f.calls, []);
});
