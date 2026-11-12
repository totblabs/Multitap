import { randomUUID } from 'node:crypto';
import { HttpError } from './errors.mjs';
import { newToken } from './security.mjs';

export const qemuConsoleProtocol = 'qemu-console-v2';
export const legacyQemuConsoleProtocol = 'qemu-console-v1';
const success = new Set(['qemu_console_active', 'qemu_console_restored']);
const failures = new Set(['qemu_console_inactive', 'qemu_console_state_missing', 'qemu_console_state_invalid', 'qemu_console_identity_changed', 'qemu_console_foreign_changed', 'qemu_console_unavailable', 'qemu_console_cancelled', 'qemu_console_busy', 'qemu_console_timeout']);
const released = new Set(['qemu_console_cancelled', 'qemu_console_timeout', 'qemu_console_identity_changed']);
const fields = ['type', 'protocol', 'operationId', 'nonce', 'ok', 'attempted', 'workerQuiescent', 'reason'];
const heldFields = ['type', 'protocol', 'operationId', 'nonce', 'sequence', 'leaseMs'];
const exact = (value, keys) => Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const leaseMs = 30000, renewMs = 10000, ackMs = 5000, maxSequence = 0xffffffff;
const repeatLimit = 6, repeatSpacingMs = 5000;

// v2 proves QEMU primary + owned outputs off, and holds that topology for this
// exact grant. Worker exit and display-hold release are separate facts. v1
// merely made QEMU active; old/offline Hosts retain the unknown PVE fallback,
// and so does a Host that answers that it could not prepare the display.
export class QemuConsolePreparation {
  constructor(relay, { timeoutMs = 25000, now = Date.now } = {}) {
    this.r = relay; this.timeoutMs = timeoutMs; this.now = now; this.operations = new Map();
  }
  supported(host) {
    return host?.ws.readyState === 1 && !host.capabilitiesPending &&
      this.now() - (host.capabilitiesAt ?? 0) <= this.r.heartbeatMs * 3 &&
      host.capabilities?.service === true && host.capabilities.qemuConsoleProtocol === qemuConsoleProtocol;
  }
  owns(grant, operation = grant.qemuOperation) {
    return !!operation && operation.grant === grant && this.operations.get(operation.id) === operation &&
      this.r.displayBusy.get(grant.desktopId) === operation;
  }
  preparation(grant) {
    const operation = grant.qemuOperation, now = this.now();
    const isolated = this.owns(grant, operation) && operation.status === 'held' &&
      this.r.hosts.get(grant.desktopId) === operation.host && this.supported(operation.host) &&
      this.r.store.authenticateHost(operation.host.tokenHash)?.id === operation.host.identity.id &&
      now < operation.leaseDeadline && (operation.sequence === operation.ackSequence || now < operation.ackDeadline);
    return { status: isolated ? 'isolated' : 'unknown' };
  }
  correlated(host, value) {
    const operation = this.operations.get(value.operationId), r = this.r;
    if (!operation || operation.host !== host || r.hosts.get(operation.desktopId) !== host ||
        !this.owns(operation.grant, operation) || value.nonce !== operation.nonce) return;
    if (r.store.authenticateHost(host.tokenHash)?.id !== host.identity.id) { r.dropHost(host, 'host_revoked'); return; }
    return operation;
  }
  send(operation, type, extra = {}) {
    try {
      if (!this.r.send(operation.host.ws, { type, protocol: qemuConsoleProtocol, operationId: operation.id, nonce: operation.nonce, ...extra })) this.r.dropHost(operation.host, 'host_backpressure');
    } catch { this.r.dropHost(operation.host, 'host_backpressure'); }
  }
  async prepare(grant) {
    const r = this.r, host = r.hosts.get(grant.desktopId);
    if (!this.supported(host) || grant.snapshot.model !== 'std') return undefined;
    const binding = r.store.hostConsoleBinding(host.tokenHash);
    if (!binding || binding.id !== host.identity.id || binding.desktop_id !== grant.desktopId ||
        binding.machine_uuid?.toLowerCase() !== grant.machineUuid) throw new HttpError(409, 'console_identity_changed');
    r.console.current(grant);
    if (r.displayBusy.size >= 500 || this.operations.size >= 500) throw new HttpError(409, 'console_busy');
    const now = this.now(), operation = { kind: 'qemu-console', id: randomUUID(), nonce: newToken(),
      userId: grant.userId, tokenHash: grant.tokenHash, desktopId: grant.desktopId, grant, host,
      status: 'pending', submittedAt: now, checkedAt: now, deadline: now + this.timeoutMs, nextPoll: now + 1000, settled: false };
    const result = new Promise((resolve, reject) => { operation.resolve = resolve; operation.reject = reject; });
    grant.qemuOperation = operation; this.operations.set(operation.id, operation); r.displayBusy.set(operation.desktopId, operation);
    // A console that is being opened waits for this answer; one that is open (resume) keeps its own deadlines.
    if (grant.state === 'preparing') grant.expiresAt = r.console.now() + this.timeoutMs + r.console.grantMs;
    this.send(operation, 'host.qemu-console.prepare', { machineUuid: grant.machineUuid, model: 'std' });
    return result;
  }
  // A console that is open when its desktop's Host comes back on a new
  // connection (Windows restarted under it) is one that Host knows nothing of:
  // it would leave every display on. The preparation is asked again once the
  // Host reports a signed-in session. Before that the Host could only park its
  // own outputs and would hold that for the rest of the console; with a
  // session it leaves only the basic display on. A repeat that is refused (a
  // Host that just started is busy for a while) or gets no answer leaves the
  // open console as it is and is asked again with a later report: no sooner
  // than five seconds after the last one, six times at most for one Host
  // connection.
  resume(host) {
    if (!this.supported(host) || host.capabilities.interactiveSession !== true) return;
    const r = this.r, desktopId = host.identity.desktop_id;
    if (r.displayBusy.has(desktopId)) return;
    for (const grant of r.console.grants.values()) {
      const last = grant.qemuOperation;
      if (grant.closed || grant.desktopId !== desktopId || grant.state === 'preparing' || !last || this.operations.has(last.id)) continue;
      // Only what a lost Host left behind, or an earlier repeat on this connection.
      const repeats = last.host === host ? last.repeat ?? 0 : 0;
      if (last.host === host && (!last.repeat || repeats >= repeatLimit || this.now() - last.submittedAt < repeatSpacingMs)) continue;
      try { r.console.current(grant); } catch { continue; }
      this.prepare(grant).catch(() => { /* The console stays open without the proof. */ });
      if (grant.qemuOperation !== last) grant.qemuOperation.repeat = repeats + 1;
      return;
    }
  }
  settle(operation, error, result) {
    if (operation.settled) return;
    operation.settled = true;
    if (error) operation.reject(new HttpError(409, error)); else operation.resolve(result);
    operation.resolve = operation.reject = null;
  }
  invalidate(operation) {
    // Capabilities arriving before the quiet release cannot remove this
    // barrier. Only a later Host packet can advertise stream readiness again.
    operation.host.capabilitiesPending = true; operation.host.capabilitiesAt = 0;
    operation.host.qemuConsoleRefreshRequired = true;
  }
  capabilities(host) {
    if (!host.qemuConsoleRefreshRequired) return true;
    if ([...this.operations.values()].some(operation => operation.host === host && operation.status === 'releasing')) return false;
    delete host.qemuConsoleRefreshRequired; return true;
  }
  finish(operation) {
    if (this.r.displayBusy.get(operation.desktopId) === operation) this.r.displayBusy.delete(operation.desktopId);
    this.operations.delete(operation.id);
  }
  cancel(operation, reason = 'console_cancelled') {
    if (!this.owns(operation.grant, operation) || !['pending', 'held'].includes(operation.status)) return;
    // A repeat for an open console (resume) that was never held ends alone: the console was open without it.
    const unansweredRepeat = !!operation.repeat && operation.status === 'pending';
    operation.status = 'releasing'; operation.error = reason === 'display_timeout' ? 'console_timeout' : reason;
    operation.checkedAt = this.now(); operation.nextPoll = this.now();
    this.invalidate(operation); this.settle(operation, operation.error);
    if (operation.grant.qemuConsolePreparation) operation.grant.qemuConsolePreparation.status = 'unknown';
    this.send(operation, 'host.qemu-console.cancel');
    // A failed lease cannot leave a console advertised as isolated. retire is
    // idempotent; its exact cancelGrant cannot send a second release.
    if (!operation.grant.closed && !unansweredRepeat) this.r.console.retire(operation.grant, operation.error);
  }
  cancelGrant(grant) { if (this.owns(grant)) this.cancel(grant.qemuOperation); }
  result(host, value) {
    const operation = this.correlated(host, value), r = this.r;
    if (!operation) return;
    const valid = exact(value, fields) && value.type === 'host.qemu-console.result' && value.protocol === qemuConsoleProtocol &&
      ['ok', 'attempted', 'workerQuiescent'].every(key => typeof value[key] === 'boolean') &&
      (value.ok ? success.has(value.reason) && value.workerQuiescent &&
        value.attempted === (value.reason === 'qemu_console_restored') : failures.has(value.reason));
    if (!valid) { this.cancel(operation, 'console_protocol_error'); r.dropHost(host, 'invalid_qemu_console_result'); return; }
    if (operation.status !== 'releasing') {
      try { r.console.current(operation.grant); }
      catch { this.cancel(operation, 'console_cancelled'); }
      if (operation.status === 'pending' && this.now() >= operation.deadline) this.cancel(operation, 'console_timeout');
      if (operation.status === 'held' && this.now() >= operation.leaseDeadline) this.cancel(operation, 'console_timeout');
    }
    operation.checkedAt = this.now();
    if (operation.status === 'releasing') {
      // A late prepare success proves worker exit, not hold release. Polling
      // repeats the exact release result until its worker is truly quiet.
      if (!value.ok && value.workerQuiescent && released.has(value.reason)) this.finish(operation);
      return;
    }
    if (operation.status === 'held') {
      if (value.ok) return; // Status/duplicate prepare cannot extend a lease.
      this.cancel(operation, 'console_display_' + value.reason.slice('qemu_console_'.length));
      if (value.workerQuiescent && released.has(value.reason)) this.finish(operation);
      return;
    }
    if (!value.workerQuiescent) { this.cancel(operation, 'console_timeout'); return; }
    if (value.ok) {
      operation.status = 'held'; operation.sequence = 0; operation.ackSequence = 0;
      // The Host starts its lease after preparation. Dispatch time is a
      // conservative lower bound even if the result spent time in transit.
      operation.leaseDeadline = operation.submittedAt + leaseMs; operation.nextRenew = operation.submittedAt + renewMs;
      operation.grant.qemuConsolePreparation = { status: 'isolated' };
      this.settle(operation, null, operation.grant.qemuConsolePreparation);
      return;
    }
    if (value.attempted) this.invalidate(operation);
    this.finish(operation); operation.status = 'failed';
    // The Host could not prepare the display: nobody to act for it in Windows,
    // or its own stop of the virtual outputs failed (an attempted park). The
    // console is what a person opens exactly then, so it still opens, without a
    // readiness claim, on whatever Proxmox shows. An attempted change has
    // already withdrawn the Host's advertised state above.
    if (value.reason === 'qemu_console_unavailable') this.settle(operation, null, { status: 'unknown' });
    else this.settle(operation, 'console_display_' + value.reason.slice('qemu_console_'.length));
  }
  held(host, value) {
    const operation = this.correlated(host, value);
    if (!operation || operation.status !== 'held') return;
    if (!exact(value, heldFields) || value.type !== 'host.qemu-console.held' || value.protocol !== qemuConsoleProtocol ||
        !Number.isInteger(value.sequence) || value.sequence < 1 || value.sequence > maxSequence || value.leaseMs !== leaseMs) {
      this.cancel(operation, 'console_protocol_error'); return;
    }
    // Neither reversed/duplicate ACKs nor an unsolicited future sequence can
    // acknowledge our one outstanding renewal or advance the broker deadline.
    if (value.sequence !== operation.sequence || value.sequence <= operation.ackSequence) return;
    if (this.now() >= operation.ackDeadline || this.now() >= operation.leaseDeadline) { this.cancel(operation, 'console_timeout'); return; }
    try { this.r.console.current(operation.grant); }
    catch { this.cancel(operation, 'console_cancelled'); return; }
    operation.ackSequence = value.sequence;
    // Use dispatch time, not receipt time: delayed ACKs cannot buy extra time.
    operation.leaseDeadline = operation.renewSentAt + leaseMs; operation.nextRenew = operation.renewSentAt + renewMs;
    operation.checkedAt = this.now();
  }
  hostLost(host) {
    for (const operation of this.operations.values()) if (operation.host === host) {
      this.finish(operation); operation.status = 'unknown';
      if (operation.grant.qemuConsolePreparation) operation.grant.qemuConsolePreparation.status = 'unknown';
      this.settle(operation, 'console_unavailable');
    }
  }
  revalidate() {
    const now = this.now();
    for (const operation of this.operations.values()) {
      if (operation.status !== 'releasing') {
        try { this.r.console.current(operation.grant); }
        catch { this.cancel(operation, 'console_cancelled'); }
      }
      if (operation.status === 'pending' && now >= operation.deadline) this.cancel(operation, 'console_timeout');
      if (operation.status === 'held') {
        if (now >= operation.leaseDeadline || (operation.sequence > operation.ackSequence && now >= operation.ackDeadline)) this.cancel(operation, 'console_timeout');
        else if (now >= operation.nextRenew && operation.sequence === operation.ackSequence) {
          if (operation.sequence >= maxSequence) this.cancel(operation, 'console_timeout');
          else {
            ++operation.sequence; operation.renewSentAt = now; operation.ackDeadline = now + ackMs;
            this.send(operation, 'host.qemu-console.renew', { sequence: operation.sequence });
          }
        }
      }
      if (operation.status !== 'releasing' || now < operation.nextPoll) continue;
      operation.nextPoll = now + 5000;
      this.send(operation, 'host.qemu-console.status');
    }
  }
}
