import { randomUUID } from 'node:crypto';
import { WebSocket } from 'ws';
import { HttpError } from './errors.mjs';
import { newToken } from './security.mjs';

export const consoleReasons = new Set(['console_ready', 'native_provider_required', 'reboot_required', 'virtual_device_not_ready', 'windows_session_unknown', 'windows_session_locked', 'windows_identity_required', 'windows_identity_mismatch', 'windows_login_required', 'windows_session_ambiguous', 'rdp_session_active', 'rdp_session_disconnected', 'console_unavailable', 'console_in_use', 'operation_cancelled', 'console_busy', 'console_timeout', 'console_transfer_failed', 'console_transfer_pending', 'console_binding_changed', 'worker_failed', 'administrator_required']);
const resultReasons = new Set([...consoleReasons].filter(reason => !['console_ready', 'rdp_session_active', 'rdp_session_disconnected'].includes(reason)));

// Console handoff shares the VM mutation lock but does not grant streaming
// readiness. Clients cannot name Windows accounts, source or target sessions.
export class ConsolePreparation {
  constructor(relay, timeoutMs = 25000) { this.relay = relay; this.timeoutMs = timeoutMs; this.operations = new Map(); this.latest = new Map(); }
  fresh(host) { const r = this.relay; return host?.ws.readyState === WebSocket.OPEN && !host.capabilitiesPending && Date.now() - (host.capabilitiesAt ?? 0) <= r.heartbeatMs * 3; }
  available(host) { return this.fresh(host) && host.capabilities?.service === true && host.capabilities.consoleRecoverySupported === true && host.capabilities.consoleRecoveryAvailable === true; }
  public(operation) { return operation ? { id: operation.id, status: operation.status, attempted: operation.attempted, error: operation.error, submittedAt: new Date(operation.submittedAt).toISOString(), checkedAt: new Date(operation.checkedAt).toISOString() } : null; }
  view(desktopId, userId, operation) {
    const r = this.relay, host = r.hosts.get(desktopId);
    operation ??= this.operations.get(this.latest.get(userId + ':' + desktopId));
    return { supported: this.fresh(host) && host.capabilities?.consoleRecoverySupported === true,
      available: !!this.available(host) && !r.displayBusy.has(desktopId),
      reason: this.fresh(host) && consoleReasons.has(host.capabilities?.consoleRecoveryReason) ? host.capabilities.consoleRecoveryReason : 'windows_session_unknown',
      busy: r.displayBusy.get(desktopId)?.kind === 'console', operation: this.public(operation) };
  }
  status(user, desktopId, operationId) {
    if (!this.relay.store.assignedDesktop(user.id, desktopId)) throw new HttpError(404, 'desktop_not_found');
    let operation;
    if (operationId !== undefined) {
      operation = this.operations.get(operationId);
      if (!operation || operation.userId !== user.id || operation.desktopId !== desktopId) throw new HttpError(404, 'console_operation_not_found');
    }
    return { consolePreparation: this.view(desktopId, user.id, operation) };
  }
  start(user, hash, desktopId, confirmation) {
    this.relay.assertAccessAdmission?.();
    const r = this.relay, binding = { userId: user.id, tokenHash: hash, desktopId };
    if (!r.authorized(binding)) throw new HttpError(404, 'desktop_not_found');
    if (confirmation !== 'resume-windows-session') throw new HttpError(400, 'console_confirmation_required');
    const host = r.hosts.get(desktopId);
    if (!host || r.store.authenticateHost(host.tokenHash)?.id !== host.identity.id) throw new HttpError(409, 'host_offline');
    if (!this.available(host)) throw new HttpError(409, 'console_recovery_unavailable');
    r.assertDisplayIdle(desktopId,'console');
    if (r.powerBusy(desktopId)) throw new HttpError(409, 'power_in_progress');
    if (r.hasSession(desktopId)) throw new HttpError(409, 'desktop_busy');
    if (r.displayBusy.size >= 500 || this.operations.size >= 10000) throw new HttpError(503, 'console_busy');
    const now = Date.now(), operation = { ...binding, kind: 'console', id: randomUUID(), nonce: newToken(), host, status: 'pending', attempted: null, error: null, submittedAt: now, checkedAt: now, deadline: now + this.timeoutMs };
    this.operations.set(operation.id, operation); this.latest.set(user.id + ':' + desktopId, operation.id); r.displayBusy.set(desktopId, operation);
    try {
      if (!r.send(host.ws, { type: 'host.console.prepare', operationId: operation.id, nonce: operation.nonce, userConfirmed: true })) r.dropHost(host, 'host_backpressure');
    } catch { r.dropHost(host, 'host_backpressure'); }
    this.audit('console_resume_requested', user.id, desktopId);
    return { consolePreparation: this.view(desktopId, user.id, operation) };
  }
  cancel(operation, reason) {
    const r = this.relay;
    if (r.displayBusy.get(operation.desktopId) !== operation || operation.status !== 'pending') return;
    operation.status = reason === 'authorization_revoked' ? 'failed' : 'unknown'; operation.error = reason === 'display_timeout' ? 'console_timeout' : reason; operation.checkedAt = Date.now();
    r.send(operation.host.ws, { type: 'host.console.cancel', operationId: operation.id, nonce: operation.nonce });
    // A lost reply does not prove whether Windows moved. No retry; retain the
    // VM lock until the exact worker completion or host loss.
  }
  result(host, value) {
    const r = this.relay, operation = this.operations.get(value.operationId);
    if (!operation || operation.host !== host || r.hosts.get(operation.desktopId) !== host || r.displayBusy.get(operation.desktopId) !== operation || value.nonce !== operation.nonce) return;
    if (r.store.authenticateHost(host.tokenHash)?.id !== host.identity.id) { r.dropHost(host, 'host_revoked'); return; }
    const systemError = value.systemError === undefined ? 0 : value.systemError;
    if (typeof value.ok !== 'boolean' || typeof value.attempted !== 'boolean' || !Number.isInteger(systemError) || systemError < 0 || systemError > 0xffffffff || (value.ok && systemError !== 0) || (value.ok ? value.reason !== '' : !resultReasons.has(value.reason))) { this.cancel(operation, 'console_result_invalid'); r.dropHost(host, 'invalid_console_result'); return; }
    if (!r.authorized(operation)) this.cancel(operation, 'authorization_revoked');
    else if (Date.now() > operation.deadline) this.cancel(operation, 'console_timeout');
    operation.attempted = value.attempted; operation.checkedAt = Date.now();
    if (operation.status === 'pending') { operation.status = value.ok ? 'succeeded' : 'failed'; operation.error = value.ok ? null : value.reason; }
    r.displayBusy.delete(operation.desktopId);
    this.audit('console_resume_completed', operation.userId, operation.desktopId + ':' + operation.status + ':' + (value.ok ? '' : value.reason) + ':windows=' + systemError);
    // A result never changes monitor/media-ready capabilities. The client
    // must wait for a fresh actual console/display/capture observation.
  }
  cleanup() {
    const r = this.relay;
    for (const [id, operation] of this.operations) if (r.displayBusy.get(operation.desktopId) !== operation && Date.now() - operation.checkedAt > 600000) {
      this.operations.delete(id); if (this.latest.get(operation.userId + ':' + operation.desktopId) === id) this.latest.delete(operation.userId + ':' + operation.desktopId);
    }
  }
  audit(action, userId, detail) { try { this.relay.store.audit(action, userId, detail); } catch { /* Audit I/O cannot retain a completed mutation lock. */ } }
}
