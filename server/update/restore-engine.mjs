// Restoring a backup (백업·복원) through the same guarded steps the updater
// uses for the server access settings (server-access-engine.mjs): the broker's
// admission fence, a bounded wait for work the broker itself started, the
// broker ending its connections, a plan made durable before the first write,
// apply, restart, health check, and automatic rollback to the safety copy.
// The broker stays the one that holds the fence and ends connections; this
// engine only observes it through the same local probe.
import { mkdir, lstat, readFile } from 'node:fs/promises';
import { join } from 'node:path';
import { atomicJson } from './engine.mjs';
import { serverAccessQuiescence } from './server-access-engine.mjs';

export const restoreProtocol = 'server-restore-v1';
export const restoreActiveStates = new Set(['waiting_idle', 'applying', 'health_check']);
export const restoreTerminalStates = new Set(['succeeded', 'rolled_back', 'failed', 'recovery_required']);
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/, hex = /^[a-f0-9]{64}$/;
const exact = (value, keys) => value && typeof value === 'object' && !Array.isArray(value) && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const fail = code => { throw Error('restore_' + code); };
const safeError = error => /^restore_[a-z_]{1,80}$/.test(error?.message) ? error.message : 'restore_failed';

class RestoreJournal {
  constructor(directory) { this.directory = directory; this.path = join(directory, 'status.json'); }
  async initialize() {
    await mkdir(this.directory, { recursive: true, mode: 0o700 });
    const stat = await lstat(this.directory);
    if (!stat.isDirectory() || stat.isSymbolicLink() || (process.platform !== 'win32' && ((stat.mode & 0o077) || stat.uid !== 0))) fail('storage_invalid');
  }
  async read() {
    try {
      const stat = await lstat(this.path);
      if (!stat.isFile() || stat.isSymbolicLink() || stat.size > 262144) fail('storage_invalid');
      return JSON.parse(await readFile(this.path, 'utf8'));
    } catch (error) { if (error.code === 'ENOENT') return null; throw Error('restore_storage_invalid'); }
  }
  write(state) { return atomicJson(this.path, state); }
}

export class ServerRestoreEngine {
  constructor({ adapter, directory, storage, otherBusy = () => false, now = Date.now, sleep = ms => new Promise(resolve => setTimeout(resolve, ms)), maxWaitMs = 120000, endWaitMs = 15000, pollMs = 1000 }) {
    this.adapter = adapter; this.storage = storage ?? new RestoreJournal(directory); this.otherBusy = otherBusy; this.now = now; this.sleep = sleep;
    this.maxWaitMs = Math.min(maxWaitMs, 300000); this.endWaitMs = Math.min(endWaitMs, 60000); this.pollMs = Math.max(1, pollMs);
    this.busy = false; this.task = Promise.resolve(); this.state = { state: 'idle' };
  }
  async initialize() {
    await this.adapter.initialize(); await this.storage.initialize();
    const saved = await this.storage.read();
    if (saved) this.state = saved;
    const interrupted = restoreActiveStates.has(this.state.state);
    if (!interrupted && this.state.state !== 'recovery_required') return;
    this.busy = true;
    try {
      if (!this.state.job) {
        // Nothing is written before the plan is in the journal.
        if (interrupted) await this.set({ state: 'failed', errorCode: 'restore_interrupted', unchangedVerified: true, rollbackVerified: false, blockers: [], waitUntil: null });
      } else {
        const restored = await this.adapter.rollback(this.state.job) === true;
        await this.set({ state: restored ? 'rolled_back' : 'recovery_required', errorCode: restored ? (interrupted ? 'restore_interrupted' : this.state.errorCode ?? 'restore_interrupted') : 'restore_recovery_required', rollbackVerified: restored, unchangedVerified: false, completedAt: new Date(this.now()).toISOString() });
      }
    } catch { await this.set({ state: 'recovery_required', errorCode: 'restore_recovery_required', rollbackVerified: false, unchangedVerified: false }); }
    finally { this.busy = false; }
  }
  async set(values) { this.state = { ...this.state, ...values }; await this.storage.write(this.state); }
  status() {
    const result = { protocol: restoreProtocol, supported: true, state: this.state.state, busy: this.busy, rollbackVerified: this.state.rollbackVerified === true, unchangedVerified: this.state.unchangedVerified === true };
    for (const key of ['operationId', 'errorCode', 'blockers', 'waitUntil', 'startedAt', 'completedAt']) if (this.state[key] !== undefined && this.state[key] !== null) result[key] = this.state[key];
    return result;
  }
  async apply(input, context) {
    if (!exact(input, ['operationId', 'database', 'vault', 'schemaVersion', 'confirm']) || input.confirm !== true || !uuid.test(input.operationId ?? '') ||
        !exact(input.database, ['sha256', 'size']) || !hex.test(input.database.sha256 ?? '') || !Number.isSafeInteger(input.database.size) ||
        !exact(input.vault, ['sha256']) || !hex.test(input.vault.sha256 ?? '') || !Number.isSafeInteger(input.schemaVersion)) fail('command_invalid');
    if (!exact(context, ['fence']) || !exact(context.fence, ['token', 'operationId']) || !hex.test(context.fence.token ?? '') || context.fence.operationId !== input.operationId) fail('fence_invalid');
    if (this.busy || this.otherBusy()) fail('busy');
    if (this.state.state === 'recovery_required') fail('recovery_required');
    if (this.state.operationId === input.operationId) fail('operation_reused');
    this.busy = true;
    let previous;
    try {
      previous = await this.adapter.current();
      await this.set({ state: 'waiting_idle', operationId: input.operationId, errorCode: null, job: null, rollbackVerified: false, unchangedVerified: false, blockers: [], waitUntil: null, startedAt: new Date(this.now()).toISOString(), completedAt: null });
    } catch (error) { this.busy = false; throw /^restore_/.test(error?.message) ? error : Error('restore_unavailable'); }
    this.task = this.perform(input, previous, context).finally(() => { this.busy = false; });
    return { accepted: true, operationId: input.operationId, state: 'waiting_idle' };
  }
  async observe(current, context) {
    const report = await this.adapter.activity(current, context.fence.token);
    if (report?.protocol !== 'server-access-activity-v1' || report.fence !== true) fail('fence_lost');
    const { blockers, open } = serverAccessQuiescence(report);
    return { blockers: blockers.map(row => ({ code: row.code.replace(/^server_access_/, 'restore_'), count: row.count })), open };
  }
  // A Host update already sent, a display or VM setting change and a power
  // task are bounded by their own time limits; they are waited for, not cut.
  async settle(current, context) {
    const deadline = this.now() + this.maxWaitMs; let shown = '[]';
    while (true) {
      const { blockers } = await this.observe(current, context);
      if (!blockers.length) { if (shown !== '[]') await this.set({ blockers: [], waitUntil: null }); return; }
      const text = JSON.stringify(blockers);
      if (text !== shown) { shown = text; await this.set({ blockers, waitUntil: new Date(deadline).toISOString() }); }
      if (this.now() >= deadline) fail('idle_timeout');
      await this.sleep(this.pollMs);
    }
  }
  // The broker ends every connection itself; two reports in a row without one.
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
  async perform(input, previous, context) {
    let job = null;
    try {
      await this.settle(previous, context);
      await this.endConnections(previous, context);
      job = await this.adapter.prepare({ operationId: input.operationId, database: input.database, vault: input.vault, schemaVersion: input.schemaVersion }, { previous });
      // The plan is in the journal before the broker is stopped or a file changes.
      await this.set({ state: 'applying', job, blockers: [], waitUntil: null });
      const last = await this.observe(previous, context);
      if (last.blockers.length || last.open) fail('activity_changed');
      await this.adapter.apply(job);
      await this.set({ state: 'health_check' });
      if (await this.adapter.verify(job) !== true) fail('health_failed');
      await this.set({ state: 'succeeded', errorCode: null, rollbackVerified: false, unchangedVerified: false, completedAt: new Date(this.now()).toISOString() });
    } catch (error) {
      let restored = false, unchanged = false;
      try { if (job) restored = await this.adapter.rollback(job) === true; else unchanged = await this.adapter.unchanged(previous, context.fence.token) === true; } catch { /* A failed observation proves nothing. */ }
      try {
        await this.set({ state: job ? (restored ? 'rolled_back' : 'recovery_required') : 'failed', errorCode: job && !restored ? 'restore_recovery_required' : safeError(error),
          ...(safeError(error) === 'restore_idle_timeout' ? { waitUntil: null } : { blockers: [], waitUntil: null }), rollbackVerified: restored, unchangedVerified: unchanged, completedAt: new Date(this.now()).toISOString() });
      } catch { this.state = { ...this.state, state: job && !restored ? 'recovery_required' : this.state.state }; }
    }
  }
}
