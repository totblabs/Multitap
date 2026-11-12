import { randomBytes, randomUUID, timingSafeEqual } from 'node:crypto';
import { HttpError } from './errors.mjs';
import { serverAccessHash } from '../update/server-access-policy.mjs';

// While an apply is restarting the broker — from the moment it ended the
// connections, and again in the new process until the result is known — a
// refused request says so the way a server update does (503 server_restarting):
// released clients wait through that and connect again by themselves. Bounded,
// so a fence that never clears goes back to the plain "pending" answer.
const restartWindowMs = 120000;

// The privileged agent can observe an exact in-memory admission fence. Tokens
// never enter a browser response, logs, files, or a user-supplied URL.
export class AccessRuntime {
  constructor({ client = null, config = null, revision = null, installationId = null, initialOperationId = null }) {
    Object.assign(this, { client, config, revision, installationId });
    if (initialOperationId !== null && !/^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/.test(initialOperationId)) throw Error('server_access_operation_invalid');
    this.lease = initialOperationId ? { token: randomBytes(32).toString('hex'), operationId: initialOperationId, restartUntil: Date.now() + restartWindowMs } : null;
    this.observing = false;
    this.timer = setInterval(() => {
      if (!this.lease || this.observing) return;
      this.observing = true;
      const lease = this.lease;
      // A fence taken for a restore (hold) is released by its own observer.
      const observed = lease.observe ? Promise.resolve().then(() => lease.observe()).then(done => { if (done === true) this.release(lease); }) : this.status({ managementOrigin: null });
      observed.catch(() => {}).finally(() => { this.observing = false; });
    }, 2000);
    this.timer.unref();
  }
  // 백업·복원 (backup.mjs) closes admission with the same fence a save of the
  // access settings uses, so the two can never overlap and the updater's probe
  // of this broker (activity, ending connections) is the one that exists.
  // `observe` answers true once the operation is over; `event` names the audit
  // row written when the connections are ended.
  hold({ operationId, actor = null, event, observe }) {
    if (!this.client) throw new HttpError(503, 'restore_unavailable');
    this.assertAdmission();
    if (typeof operationId !== 'string' || !/^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/.test(operationId) || typeof observe !== 'function') throw Error('server_access_operation_invalid');
    const lease = { token: randomBytes(32).toString('hex'), operationId, actor, event, observe };
    this.lease = lease;
    return lease;
  }
  release(lease) { if (lease && this.lease === lease) this.lease = null; }
  close() { clearInterval(this.timer); this.lease = null; }
  get blocked() { return this.lease !== null; }
  assertAdmission() {
    if (!this.lease) return;
    throw Date.now() < (this.lease.restartUntil ?? 0) ? new HttpError(503, 'server_restarting') : new HttpError(409, 'server_access_pending');
  }
  accepts(token) {
    if (!this.lease || typeof token !== 'string' || !/^[a-f0-9]{64}$/.test(token)) return false;
    return timingSafeEqual(Buffer.from(token), Buffer.from(this.lease.token));
  }
  // The fence holder is about to have the connections ended: who saved it
  // (for the audit row), and from now on refusals read as a restart.
  connectionsEnding() {
    if (!this.lease) return null;
    this.lease.restartUntil = Date.now() + restartWindowMs;
    return { operationId: this.lease.operationId, actor: this.lease.actor ?? 'server', event: this.lease.event ?? 'server_access_connections_ended' };
  }
  async status(context) {
    // deploymentKind lets the console show a Compose install its .env lines.
    if (!this.client) return { protocol: 'server-access-v1', revision: this.revision ?? (this.config ? serverAccessHash(this.config) : 'bootstrap'), applied: this.config, pending: null, supported: false, errorCode: 'server_access_unavailable', managementOrigin: context.managementOrigin, bootstrapListener: context.listener ?? null, deploymentKind: context.deploymentKind ?? 'unknown', udpPort: context.udpPort ?? null };
    const state = await this.client.status();
    // Only a terminal observation of this very operation releases the fence.
    const terminal = state.state === 'succeeded' || (state.state === 'rolled_back' && state.rollbackVerified === true) || (state.state === 'failed' && state.unchangedVerified === true);
    if (this.lease?.operationId === state.operationId && terminal) this.lease = null;
    const phase = state.state === 'succeeded' ? 'applied' : state.state === 'rolled_back' && state.rollbackVerified === true ? 'failed_rolled_back' : state.state;
    return { ...state, applied: state.config ?? null, state: phase, pending: state.pendingConfig ? { config: state.pendingConfig, state: phase, operationId: state.operationId, ...(state.errorCode ? { errorCode: state.errorCode } : {}) } : null, supported: state.managed === true && state.applySupported === true, managementOrigin: context.managementOrigin, bootstrapListener: context.listener ?? null, udpPort: context.udpPort ?? null };
  }
  async test(input, context) {
    if (!this.client) throw new HttpError(503, 'server_access_unavailable');
    this.assertAdmission();
    return this.client.test(input, context);
  }
  async save(input, context, { actor = null } = {}) {
    if (!this.client) throw new HttpError(503, 'server_access_unavailable');
    this.assertAdmission();
    const lease = { token: randomBytes(32).toString('hex'), operationId: randomUUID(), actor };
    this.lease = lease;
    try {
      const result = await this.client.save(input, { ...context, fence: { token: lease.token, operationId: lease.operationId } });
      if (result.operationId !== lease.operationId) throw new HttpError(503, 'server_access_result_unknown');
      return result;
    } catch (error) {
      // A dropped acknowledgement may follow acceptance. Keep admission closed
      // until authoritative status reconciles it; do not replay save.
      if ([400, 403, 409, 422].includes(error?.status)) this.lease = null;
      throw error;
    }
  }
  // Withdraws exactly the operation this broker is fenced for. The fence is
  // released only by the engine's verified "unchanged" result, as for any
  // other end; a refusal (already applying) leaves everything as it is.
  async cancel(operationId) {
    if (!this.client) throw new HttpError(503, 'server_access_unavailable');
    if (!this.lease || this.lease.operationId !== operationId) throw new HttpError(409, 'server_access_cancel_unavailable');
    const result = await this.client.cancel({ operationId });
    await this.status({ managementOrigin: null }).catch(() => {});
    return result;
  }
}
