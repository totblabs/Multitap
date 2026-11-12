import { randomBytes, createHash } from 'node:crypto';
import { mkdir, lstat, readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { atomicJson } from './engine.mjs';
import { normalizeBrokerOrigin } from './transport-policy.mjs';
import { normalizeServerAccess, allowedServerOrigins, serverAccessHash, serverAccessProtocol } from './server-access-policy.mjs';

export const serverAccessActiveStates = new Set(['testing', 'waiting_idle', 'applying', 'health_check']);
export const serverAccessTerminalStates = new Set(['succeeded', 'rolled_back', 'failed', 'recovery_required']);
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/;
const hex = /^[a-f0-9]{64}$/;
const exact = (value, keys) => value && typeof value === 'object' && !Array.isArray(value) && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const fail = code => { throw Error('server_access_' + code); };
const safeError = error => /^server_access_[a-z_]{1,80}$/.test(error?.message) ? error.message : 'server_access_failed';
const hash = text => createHash('sha256').update(text).digest('hex');
export function serverAccessSnapshotMatches(state, current) {
  if (!current || state.appliedRevision !== current.revision || !Object.hasOwn(state, 'appliedConfig')) return false;
  try { if ((state.appliedConfig === null) !== (current.config === null) || (current.config && serverAccessHash(state.appliedConfig) !== serverAccessHash(current.config))) return false; }
  catch { return false; }
  const legacy = typeof state.appliedLegacy === 'boolean' ? state.appliedLegacy : state.state === 'succeeded' ? false : state.job?.previous?.legacy;
  if (typeof legacy === 'boolean' && legacy !== current.legacy) return false;
  const identity = state.appliedInstallationId ?? state.job?.installationId;
  return identity === undefined || identity === null || identity === current.installationId;
}

const counter = number => Number.isSafeInteger(number) && number >= 0;
function verifiedActivity(value) {
  const groups = { sessions: ['total', 'stream', 'attached', 'tickets'], channels: ['clipboard', 'clipboardTransfers', 'drive', 'driveShares', 'driveRequests', 'usb', 'usbDevices', 'print', 'printTransfers'], operations: ['display', 'sessionCleanup', 'hostUpdates', 'power'], hosts: ['connected'], live: ['channels'] };
  if (!exact(value, ['protocol', ...Object.keys(groups), 'sinceActivityMs', 'uptimeMs']) || value.protocol !== 'activity-v1') return false;
  for (const [name, fields] of Object.entries(groups)) if (!exact(value[name], fields) || fields.some(field => !counter(value[name][field]))) return false;
  return counter(value.sinceActivityMs) && counter(value.uptimeMs) && value.sinceActivityMs <= value.uptimeMs && value.sessions.total === value.sessions.stream && value.sessions.total === value.sessions.attached + value.sessions.tickets;
}
// What a fenced activity report means for a saved change (the server
// comes first: saving ends the connections).
// blockers: only work the broker itself started and a restart would cut in
//   half — a Host update already sent to its Host, a display or VM setting
//   change, a power task. Each is bounded by its own timeout.
// open: remote connections, their side channels and open Consoles. They are
//   ended by the broker, never waited for.
// A connection that just started or ended, and a Host still tidying up after
// a stop, hold nothing back (a server update does not wait for them either).
export function serverAccessQuiescence(report) {
  const value = report?.activity, connections = report?.connections;
  // activity-v1 counts open Consoles inside operations.display; the report's
  // own connection counts say how many of those they are.
  if (!verifiedActivity(value) || !exact(connections, ['people', 'sessions', 'consoles']) || Object.values(connections).some(number => !counter(number)) ||
    connections.sessions !== value.sessions.total || connections.consoles > value.operations.display) return { blockers: [{ code: 'server_access_activity_unverified', count: 0 }], open: false };
  const blockers = [], settings = value.operations.display - connections.consoles;
  if (value.operations.hostUpdates) blockers.push({ code: 'server_access_host_updates_busy', count: value.operations.hostUpdates });
  if (settings) blockers.push({ code: 'server_access_desktop_settings_busy', count: settings });
  if (value.operations.power) blockers.push({ code: 'server_access_power_busy', count: value.operations.power });
  return { blockers, open: value.sessions.total + connections.consoles > 0 || Object.values(value.channels).some(Boolean) };
}

class AccessJournal {
  constructor(directory) { this.directory = directory; this.path = join(directory, 'status.json'); }
  async initialize() {
    await mkdir(this.directory, { recursive: true, mode: 0o700 });
    const stat = await lstat(this.directory);
    if (!stat.isDirectory() || stat.isSymbolicLink() || (stat.mode & 0o077) || (process.platform !== 'win32' && stat.uid !== 0)) fail('storage_invalid');
  }
  async read() {
    try {
      const stat = await lstat(this.path);
      if (!stat.isFile() || stat.isSymbolicLink() || stat.size > 262144 || (stat.mode & 0o077) || (process.platform !== 'win32' && stat.uid !== 0)) fail('storage_invalid');
      return JSON.parse(await readFile(this.path, 'utf8'));
    } catch (error) { if (error.code === 'ENOENT') return null; throw Error('server_access_storage_invalid'); }
  }
  write(state) { return atomicJson(this.path, state); }
}

export class ServerAccessEngine {
  constructor({ adapter, directory, storage, otherBusy = () => false, now = Date.now, sleep = ms => new Promise(resolve => setTimeout(resolve, ms)), testTtlMs = 300000, maxWaitMs = 300000, endWaitMs = 15000, pollMs = 1000 }) {
    this.adapter = adapter; this.storage = storage ?? new AccessJournal(directory); this.otherBusy = otherBusy; this.now = now; this.sleep = sleep;
    this.testTtlMs = Math.min(testTtlMs, 300000); this.maxWaitMs = Math.min(maxWaitMs, 300000); this.endWaitMs = Math.min(endWaitMs, 60000); this.pollMs = Math.max(1, pollMs);
    this.tests = new Map(); this.busy = false; this.task = Promise.resolve(); this.state = { state: 'idle', pendingConfig: null };
    this.cancelRequested = false; this.committed = false; this.wake = null;
  }
  async initialize() {
    await this.adapter.initialize(); await this.storage.initialize();
    const saved = await this.storage.read();
    if (saved) this.state = saved;
    if (serverAccessActiveStates.has(this.state.state)) {
      this.busy = true;
      try {
        if (this.state.job && ['applying', 'health_check'].includes(this.state.state)) {
          const restored = await this.adapter.rollback(this.state.job);
          await this.set({ state: restored ? 'rolled_back' : 'recovery_required', rollbackVerified: restored === true, unchangedVerified: false, errorCode: restored ? 'server_access_interrupted' : 'server_access_recovery_required', pendingConfig: restored ? null : this.state.pendingConfig, plannedDockerPorts: restored ? [] : this.state.plannedDockerPorts });
          if (restored) await this.finishMarker(this.state.job);
        } else {
          const current = await this.adapter.current();
          const previous = { ...current, config: this.state.appliedConfig, revision: this.state.appliedRevision };
          const unchanged = !this.state.job && current.revision === previous.revision && await this.adapter.unchangedAfterRestart(previous) === true;
          await this.set({ state: unchanged ? 'failed' : 'recovery_required', errorCode: unchanged ? 'server_access_interrupted' : 'server_access_recovery_required', unchangedVerified: unchanged, pendingConfig: unchanged ? null : this.state.pendingConfig, plannedDockerPorts: unchanged ? [] : this.state.plannedDockerPorts });
        }
      } catch { await this.set({ state: 'recovery_required', rollbackVerified: false, unchangedVerified: false, errorCode: 'server_access_recovery_required' }); }
      finally { this.busy = false; }
    }
    if (!Object.hasOwn(this.state, 'appliedConfig')) {
      const current = await this.adapter.current();
      await this.set({ appliedConfig: current.config, appliedRevision: current.revision, appliedLegacy: current.legacy === true, appliedInstallationId: current.installationId ?? null, appliedDockerPorts: current.dockerPorts ?? [], pendingConfig: null });
    }
    this.lastCurrent = await this.adapter.current();
    if (!serverAccessSnapshotMatches(this.state, this.lastCurrent)) await this.storageChanged('server_access_storage_changed');
    else if (!Object.hasOwn(this.state, 'appliedLegacy') || !Object.hasOwn(this.state, 'appliedInstallationId'))
      await this.set({ appliedLegacy: this.lastCurrent.legacy === true, appliedInstallationId: this.lastCurrent.installationId ?? null });
  }
  async set(values) { this.state = { ...this.state, ...values }; await this.storage.write(this.state); }
  async storageChanged(code) {
    this.tests.clear();
    if (this.state.state !== 'recovery_required' || this.state.errorCode !== code || this.state.rollbackVerified || this.state.unchangedVerified)
      await this.set({ state: 'recovery_required', errorCode: code, rollbackVerified: false, unchangedVerified: false });
  }
  async mutationCurrent() {
    let current;
    try { current = await this.adapter.current(); }
    catch (error) { await this.storageChanged(safeError(error)); fail('recovery_required'); }
    if (!serverAccessSnapshotMatches(this.state, current)) { await this.storageChanged('server_access_storage_changed'); fail('recovery_required'); }
    return current;
  }
  async status() {
    let current = this.busy ? this.lastCurrent : null, observed = !this.busy, observationError = null;
    if (!current) {
      try { current = await this.adapter.current(); this.lastCurrent = current; }
      catch (error) { if (!this.lastCurrent) throw Error('server_access_unavailable'); current = this.lastCurrent; observed = false; observationError = safeError(error); }
    }
    if (!this.busy && (observationError || !serverAccessSnapshotMatches(this.state, current))) await this.storageChanged(observationError ?? 'server_access_storage_changed');
    const unverified = this.state.state === 'recovery_required';
    const result = { protocol: serverAccessProtocol, managed: current.managed === true, applySupported: !unverified && current.applySupported === true, deploymentKind: current.deploymentKind,
      revision: unverified && observed ? current.revision : this.state.appliedRevision ?? current.revision,
      config: unverified ? (observed ? current.config : null) : Object.hasOwn(this.state, 'appliedConfig') ? this.state.appliedConfig : current.config,
      pendingConfig: this.state.pendingConfig ?? null, state: this.state.state, legacy: observed && current.legacy === true, scope: unverified ? 'unverified' : this.state.state === 'succeeded' ? 'applied' : 'preflight',
      testing: this.testing === true,
      dockerPorts: unverified ? (observed ? current.dockerPorts ?? [] : []) : this.state.appliedDockerPorts ?? current.dockerPorts ?? [], plannedDockerPorts: this.state.plannedDockerPorts ?? [],
      rollbackVerified: this.state.rollbackVerified === true, unchangedVerified: this.state.unchangedVerified === true, requiresProxyConfiguration: current.requiresProxyConfiguration === true };
    result.admissionMarkerCleared = this.state.admissionMarkerCleared !== false;
    if (unverified) { result.currentObserved = observed; result.currentRevision = observed ? current.revision : null; result.lastKnownConfig = this.state.appliedConfig ?? null; result.lastKnownRevision = this.state.appliedRevision ?? null; }
    for (const key of ['operationId', 'errorCode', 'checks', 'idleReasons', 'blockers', 'waitUntil', 'completedAt']) if (this.state[key] !== undefined && this.state[key] !== null) result[key] = this.state[key];
    return result;
  }
  context(value, current, { save = false } = {}) {
    if (!value || typeof value !== 'object' || Array.isArray(value) || Object.keys(value).some(key => !['managementOrigin', 'fence'].includes(key))) fail('context_invalid');
    let managementOrigin;
    try { managementOrigin = normalizeBrokerOrigin(value.managementOrigin, { allowInsecureHttp: true }); } catch { fail('management_origin_invalid'); }
    if (current.config && !allowedServerOrigins(current.config).includes(managementOrigin)) fail('management_origin_changed');
    if (save && (!exact(value.fence, ['token', 'operationId']) || !hex.test(value.fence.token ?? '') || !uuid.test(value.fence.operationId ?? ''))) fail('fence_invalid');
    return { managementOrigin, ...(save ? { fence: value.fence } : {}) };
  }
  enter() { if (this.busy || this.otherBusy()) fail('busy'); if (this.state.state === 'recovery_required' || (this.state.state === 'failed' && this.state.operationId && !this.state.unchangedVerified)) fail('recovery_required'); this.busy = true; }
  async test(input, context) {
    if (!exact(input, ['config', 'baseRevision'])) fail('command_invalid');
    this.enter();
    this.testing = true;
    try {
      const current = await this.mutationCurrent(), config = normalizeServerAccess(input.config), trusted = this.context(context, current);
      if (input.baseRevision !== current.revision) fail('revision_changed');
      const result = await this.adapter.preflight(config, current), configHash = serverAccessHash(config), expires = this.now() + this.testTtlMs;
      if (result?.passed !== true || !Array.isArray(result.checks)) fail('test_failed');
      if ((await this.adapter.current()).revision !== current.revision) fail('revision_changed');
      for (const [id, test] of this.tests) if (test.expires <= this.now()) this.tests.delete(id);
      while (this.tests.size >= 8) this.tests.delete(this.tests.keys().next().value);
      const testId = hash(JSON.stringify({ configHash, baseRevision: current.revision, expires, nonce: randomBytes(32).toString('hex') }));
      this.tests.set(testId, { configHash, baseRevision: current.revision, expires, managementOrigin: trusted.managementOrigin, checks: result.checks, plannedDockerPorts: result.plannedDockerPorts ?? [] });
      return { testId, baseRevision: current.revision, configHash, expiresAt: new Date(expires).toISOString(), scope: 'preflight', checks: result.checks, plannedDockerPorts: result.plannedDockerPorts ?? [], requiresRestart: true };
    // Preflight is nonmutating. Preserve the prior operationId/terminal proof
    // and rollback reason so a dropped earlier save response can reconcile.
    } finally { this.testing = false; this.busy = false; }
  }
  async save(input, context) {
    if (!exact(input, ['config', 'testId', 'baseRevision', 'confirm']) || input.confirm !== true || !hex.test(input.testId ?? '')) fail('confirmation_required');
    this.enter();
    let current, config, trusted, test;
    try {
      if (this.state.admissionMarkerCleared === false && this.state.job) {
        if (!await this.finishMarker(this.state.job)) fail('prior_marker_pending');
      }
      current = await this.mutationCurrent(); config = normalizeServerAccess(input.config); trusted = this.context(context, current, { save: true }); test = this.tests.get(input.testId);
      this.lastCurrent = current;
      if (input.baseRevision !== current.revision) fail('revision_changed');
      if (!test || test.expires <= this.now() || test.baseRevision !== current.revision || test.configHash !== serverAccessHash(config) || test.managementOrigin !== trusted.managementOrigin) fail('test_required');
      if (trusted.fence.operationId === this.state.operationId) fail('operation_reused');
      // Clearing every earlier test also prevents an A→B→A revision from
      // reviving another administrator's old, still-unexpired draft token.
      this.tests.clear();
      this.cancelRequested = false; this.committed = false;
      await this.set({ state: 'waiting_idle', operationId: trusted.fence.operationId, appliedConfig: current.config, appliedRevision: current.revision, appliedLegacy: current.legacy === true, appliedInstallationId: current.installationId ?? null, appliedDockerPorts: current.dockerPorts ?? [], pendingConfig: config, plannedDockerPorts: test.plannedDockerPorts, checks: test.checks, errorCode: null, job: null, rollbackVerified: false, unchangedVerified: false, idleReasons: [], blockers: [], waitUntil: null });
    } catch (error) { this.busy = false; throw error; }
    this.task = this.perform(config, current, trusted).finally(() => { this.busy = false; });
    return { accepted: true, operationId: trusted.fence.operationId, state: 'waiting_idle' };
  }
  // Withdraws a saved change that is still waiting for the broker's own work.
  // Nothing has been ended or written yet: the ordinary failure path proves
  // the installation unchanged, which is what releases the broker's fence.
  async cancel(input) {
    if (!exact(input, ['operationId']) || !uuid.test(input.operationId ?? '')) fail('command_invalid');
    if (!this.busy || this.state.state !== 'waiting_idle' || this.state.operationId !== input.operationId) fail('cancel_unavailable');
    if (this.committed) fail('cancel_too_late');
    this.cancelRequested = true; this.wake?.();
    await this.task;
    return { accepted: true, operationId: input.operationId, state: this.state.state, cancelled: this.state.operationId === input.operationId && this.state.errorCode === 'server_access_cancelled' };
  }
  async observe(current, context) {
    const report = await this.adapter.activity(current, context.fence.token);
    if (report?.protocol !== 'server-access-activity-v1' || report.fence !== true) fail('fence_lost');
    return serverAccessQuiescence(report);
  }
  pause(ms) { return Promise.race([this.sleep(ms), new Promise(resolve => { this.wake = resolve; })]); }
  // Stage 1: a bounded, cancellable wait for the broker's own unfinished work
  // only. Connections stay up meanwhile, so a cancel disturbs nobody.
  async settle(current, context) {
    const deadline = this.now() + this.maxWaitMs; let shown = '[]';
    while (true) {
      if (this.cancelRequested) fail('cancelled');
      const { blockers } = await this.observe(current, context);
      // No await between this check and the commit: a cancel is either seen
      // here or refused as too late.
      if (this.cancelRequested) fail('cancelled');
      if (!blockers.length) { this.committed = true; if (shown !== '[]') await this.set({ idleReasons: [], blockers: [], waitUntil: null }); return; }
      const text = JSON.stringify(blockers);
      // The journal keeps what blocked it, so a timeout can name it.
      if (text !== shown) { shown = text; await this.set({ idleReasons: blockers.map(row => row.code), blockers, waitUntil: new Date(deadline).toISOString() }); }
      if (this.now() >= deadline) fail('idle_timeout');
      await this.pause(this.pollMs);
    }
  }
  // Stage 2: the broker ends every remaining connection itself and the engine
  // goes on once two consecutive reports show none.
  async endConnections(current, context) {
    const deadline = this.now() + this.endWaitMs;
    while (true) {
      const first = await this.observe(current, context);
      if (first.blockers.length) fail('activity_changed');
      if (!first.open) {
        await this.sleep(Math.min(250, this.pollMs));
        const second = await this.observe(current, context);
        if (second.blockers.length) fail('activity_changed');
        if (!second.open) return;
      }
      if (this.now() >= deadline) fail('connections_busy');
      const ended = await this.adapter.endConnections(current, context.fence.token);
      if (ended?.protocol !== 'server-access-connections-v1' || ended.fence !== true) fail('fence_lost');
      await this.sleep(Math.min(250, this.pollMs));
    }
  }
  async perform(config, previous, context) {
    let job = null;
    try {
      await this.settle(previous, context);
      await this.endConnections(previous, context);
      if ((await this.adapter.current()).revision !== previous.revision) fail('revision_changed');
      job = await this.adapter.prepare(config, { previous, operationId: context.fence.operationId });
      // Persist the immutable rollback plan BEFORE the first configuration write.
      await this.set({ state: 'applying', job, idleReasons: [], blockers: [], waitUntil: null });
      const last = await this.adapter.activity(previous, context.fence.token);
      const quiet = last?.protocol === 'server-access-activity-v1' && last.fence === true ? serverAccessQuiescence(last) : null;
      if (!quiet || quiet.blockers.length || quiet.open) fail('activity_changed');
      await this.adapter.apply(job);
      await this.set({ state: 'health_check' });
      if (await this.adapter.verify(job) !== true) fail('health_failed');
      const applied = await this.adapter.current(); this.lastCurrent = applied;
      await this.set({ state: 'succeeded', appliedConfig: config, appliedRevision: job.revision, appliedLegacy: applied.legacy === true, appliedInstallationId: applied.installationId ?? null, appliedDockerPorts: applied.dockerPorts ?? [], plannedDockerPorts: [], pendingConfig: null, errorCode: null, completedAt: new Date(this.now()).toISOString(), rollbackVerified: false, unchangedVerified: false });
      await this.finishMarker(job);
    } catch (error) {
      let restored = false, unchanged = false;
      try { if (job) restored = await this.adapter.rollback(job) === true; else unchanged = await this.adapter.unchanged(previous, context.fence.token) === true; } catch { /* Do not imply recovery from a failed observation. */ }
      const errorCode = job && !restored ? 'server_access_recovery_required' : safeError(error);
      await this.set({ state: job ? (restored ? 'rolled_back' : 'recovery_required') : 'failed', errorCode,
        // A timeout keeps what it waited for; every other end clears it.
        ...(errorCode === 'server_access_idle_timeout' ? { waitUntil: null } : { idleReasons: [], blockers: [], waitUntil: null }),
        pendingConfig: restored || unchanged ? null : config, plannedDockerPorts: restored || unchanged ? [] : this.state.plannedDockerPorts, rollbackVerified: restored, unchangedVerified: unchanged, completedAt: new Date(this.now()).toISOString() });
      if (restored) { this.lastCurrent = previous; await this.finishMarker(job); }
    }
  }
  async finishMarker(job) {
    let cleared = false;
    try { cleared = await this.adapter.clearAdmissionMarker(job) === true; } catch { /* Keep the durable verified terminal result. */ }
    try { await this.set({ admissionMarkerCleared: cleared }); } catch { this.state = { ...this.state, admissionMarkerCleared: cleared }; }
    // A marker-cleanup bookkeeping failure must never roll back already
    // verified settings after the broker may have reopened admission.
    return cleared;
  }
}
