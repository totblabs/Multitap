import { randomUUID } from 'node:crypto';
import { WebSocket } from 'ws';
import { HttpError } from './errors.mjs';
import { readReleases, compareReleaseVersions } from './downloads.mjs';

// Assigned-user and administrator Host program updates (host-update-v1). The broker
// sends only metadata from its own release catalog; the Host downloads from
// its configured broker origin, verifies size/SHA-256 and runs its installer
// unattended. Released hosts never announce the protocol and are left alone.
export const hostUpdateProtocol = 'host-update-v1';
export const hostUpdateMinimumVersion = '0.2.59';
// Hosts that announce this in host.update.hello verify the project's release
// signature and receive it with each request; older Hosts accept exactly the
// unsigned field set and never see it. The ack names it so such a Host knows
// this broker understands the signature_invalid code.
export const hostUpdateReleaseSignature = 'multitap-windows-release-signature-v1';
const uuid = /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/;
const versionPattern = /^\d{1,6}\.\d{1,6}\.\d{1,6}$/;
const hostStates = new Set(['accepted', 'downloading', 'verifying', 'installing', 'completed', 'failed', 'deferred', 'rejected']);
// Fixed Host/broker vocabulary. Free text from a Host never reaches a page.
export const hostUpdateCodes = new Set(['', 'invalid_request', 'busy', 'not_newer', 'same_version', 'session_active', 'download_failed',
  'download_size_mismatch', 'hash_mismatch', 'not_executable', 'stage_failed', 'launch_failed', 'install_failed', 'install_result_missing',
  'certificate_trust_required', 'driver_install_required', 'existing_install_required', 'display_attention', 'restart_required',
  'result_unreported', 'host_no_response', 'host_no_progress', 'host_not_returned', 'host_disconnected', 'wait_expired', 'signature_invalid']);
const active = new Set(['waiting', 'dispatched', 'accepted', 'downloading', 'verifying', 'installing']);
const inFlight = new Set(['dispatched', 'accepted', 'downloading', 'verifying', 'installing']);
const reportable = new Set([...active, 'unknown']);
const auditEvents = { completed: 'host_update_completed', failed: 'host_update_failed', rejected: 'host_update_rejected', unknown: 'host_update_unknown', expired: 'host_update_expired' };
const integer = (value, max) => Number.isSafeInteger(value) && value >= 0 && value <= max;
// hostMissingAfterUpdate: the attempts whose installer ran or may have run —
// completed, failed, or unanswered once sent. A refusal, an expired wait and
// host_disconnected (the request never reached the Host) leave the Host as it was.
const missingAttempt = (status, code) => status === 'completed' || status === 'failed' || (status === 'unknown' && code !== 'host_disconnected');
const auditStatus = Object.fromEntries(Object.entries(auditEvents).map(([status, event]) => [event, status]));
const auditResult = /^([A-Za-z0-9_-]{1,128}):(\d{1,6}\.\d{1,6}\.\d{1,6})>(\d{1,6}\.\d{1,6}\.\d{1,6}):([a-z_]{1,48}|-)(?::restart_required)?$/;

// Whether a Host that is not connected now went away with the last update
// attempt and stayed away: last heard from no later than linkMs after the
// attempt ended (later, its absence is not the update's to explain), and
// silent for missingMs since the later of the two (an ordinary restart of
// the Host service is back well within it).
export function hostMissingAfterUpdate({ now, outcome, lastSeenAt, missingMs, linkMs }) {
  if (!outcome || !Number.isFinite(outcome.endedAt) || !Number.isFinite(lastSeenAt)) return false;
  if (lastSeenAt > outcome.endedAt + linkMs) return false;
  return now - Math.max(outcome.endedAt, lastSeenAt) >= missingMs;
}

export class HostUpdates {
  constructor(relay, { downloadDir, catalog, now = Date.now, tickMs = 1000, dispatchMs = 30000, progressMs = 5 * 60000, installMs = 20 * 60000, waitMs = 24 * 3600000, retryMs = 30000, catalogMs = 10000, missingMs = 3 * 60000, linkMs = 10 * 60000, onChange = () => {} } = {}) {
    Object.assign(this, { relay, now, dispatchMs, progressMs, installMs, waitMs, retryMs, catalogMs, missingMs, linkMs, onChange });
    this.catalog = catalog ?? (() => readReleases(downloadDir));
    this.operations = new Map(); this.cache = null;
    // The last finished attempt per desktop, as long as it was the last one.
    this.outcomes = this.restore();
    this.timer = setInterval(() => this.tick(), tickMs); this.timer.unref();
  }
  close() { clearInterval(this.timer); }
  changed() { try { this.onChange(); } catch { /* A watcher cannot fail maintenance bookkeeping. */ } }
  // Latest published Host installer of this installation, briefly cached.
  async releases() {
    const now = this.now();
    if (!this.cache || now - this.cache.at > this.catalogMs) {
      let entries = [];
      try { entries = await this.catalog(); } catch { entries = []; }
      const hosts = (Array.isArray(entries) ? entries : []).filter(entry => entry?.product === 'host' && entry.format === 'installer' && versionPattern.test(entry.version) &&
        entry.filename === `Multitap-Host-${entry.version}-windows-x64.exe` && entry.url === '/downloads/' + entry.filename && /^[a-f0-9]{64}$/.test(entry.sha256) && integer(entry.size, 128 * 1024 * 1024) && entry.size > 0);
      let latest = null;
      for (const entry of hosts) if (!latest || compareReleaseVersions(entry.version, latest.version) > 0) latest = entry;
      this.cache = { at: now, latest, all: hosts };
    }
    return this.cache;
  }
  authenticated(host, desktopId = host?.identity?.desktop_id) {
    try {
      if (!host || host.identity.desktop_id !== desktopId || this.relay.hosts.get(desktopId) !== host || host.ws.readyState !== WebSocket.OPEN) return false;
      const current = this.relay.store.authenticateHost(host.tokenHash);
      return !!current && current.id === host.identity.id && current.desktop_id === desktopId;
    } catch { return false; }
  }
  supported(host) { return host?.ws.readyState === WebSocket.OPEN && host.updateProtocol === hostUpdateProtocol; }
  sessionActive(desktopId) { for (const session of this.relay.sessions.values()) if (session.desktopId === desktopId) return true; return false; }
  busy(desktopId) { const r = this.relay; return r.accessPending?.() === true || this.sessionActive(desktopId) || r.vmConfigBusy?.(desktopId) === true || r.displayBusy.has(desktopId) || !!r.powerBusy(desktopId) || r.sessionStop.blocked(desktopId); }
  // Sessions, display/console changes and power actions wait for a dispatched
  // update; a disconnecting request also holds them while it waits.
  guard(desktopId) {
    const operation = this.operations.get(desktopId);
    if (operation && (inFlight.has(operation.status) || (operation.status === 'waiting' && operation.disconnectSessions))) throw new HttpError(409, 'host_update_in_progress');
  }
  public(operation) {
    if (!operation) return null;
    return { id: operation.id, status: operation.status, code: operation.code, fromVersion: operation.fromVersion, targetVersion: operation.targetVersion,
      progress: operation.progress, rebootRequired: operation.rebootRequired, disconnectSessions: operation.disconnectSessions,
      submittedAt: new Date(operation.submittedAt).toISOString(), checkedAt: new Date(operation.checkedAt).toISOString() };
  }
  view(desktopId, latest) {
    const host = this.relay.hosts.get(desktopId), connected = this.authenticated(host, desktopId);
    const version = connected && versionPattern.test(host.identity.version ?? '') ? host.identity.version : null;
    const operation = this.operations.get(desktopId), latestVersion = latest?.version ?? null;
    const newer = !!(version && latestVersion && compareReleaseVersions(latestVersion, version) > 0);
    const supported = connected && this.supported(host);
    return { protocol: hostUpdateProtocol, minimumVersion: hostUpdateMinimumVersion, connected, supported, version, latestVersion, newer,
      available: supported && newer && !(operation && active.has(operation.status)), sessionActive: this.sessionActive(desktopId), operation: this.public(operation) };
  }
  async views(desktopIds) {
    const { latest } = await this.releases();
    const desktops = Object.fromEntries(desktopIds.map(id => [id, this.view(id, latest)]));
    return { latest, desktops, availableCount: Object.values(desktops).filter(view => view.available).length };
  }
  // ownSessionsOnly: the requesting account's id when it may end only its own
  // sessions (a non-administrator); re-checked after the catalog await, right
  // before the synchronous disconnect.
  async request({ actor, desktopId, version, disconnectSessions = false, allowDowngrade = false, source = 'admin', ownSessionsOnly, authorize }) {
    this.relay.assertAccessAdmission?.();
    if (typeof version !== 'string' || !versionPattern.test(version)) throw new HttpError(400, 'invalid_host_update_version');
    if (typeof disconnectSessions !== 'boolean' || typeof allowDowngrade !== 'boolean') throw new HttpError(400, 'invalid_host_update_request');
    if (source !== 'admin' && allowDowngrade) throw new HttpError(400, 'invalid_host_update_request');
    const { latest, all } = await this.releases();
    // Catalog I/O is an authorization boundary too. In particular, a client
    // administrator may have become an ordinary account while it was pending.
    // Recompute the scope before storing an operation or ending any session.
    if (authorize !== undefined) {
      const authority = authorize();
      ownSessionsOnly = authority.ownSessionsOnly;
    }
    this.relay.assertAccessAdmission?.();
    const r = this.relay, host = r.hosts.get(desktopId);
    if (!host || host.ws.readyState !== WebSocket.OPEN || r.store.authenticateHost(host.tokenHash)?.id !== host.identity.id) throw new HttpError(409, 'host_offline');
    if (!this.supported(host)) throw new HttpError(409, 'host_update_unsupported', 'host_update_unsupported', { minimumVersion: hostUpdateMinimumVersion });
    const release = all.find(entry => entry.version === version);
    if (!release || (source !== 'admin' && release !== latest)) throw new HttpError(409, 'host_release_unavailable');
    const running = host.identity.version;
    if (typeof running !== 'string' || !versionPattern.test(running)) throw new HttpError(409, 'host_version_unknown');
    const order = compareReleaseVersions(version, running);
    if (order === 0) throw new HttpError(409, 'host_update_current');
    if (order < 0 && !allowDowngrade) throw new HttpError(409, 'host_update_not_newer');
    if (disconnectSessions && ownSessionsOnly !== undefined && [...r.sessions.values()].some(session => session.desktopId === desktopId && session.userId !== ownSessionsOnly)) throw new HttpError(409, 'desktop_busy', 'desktop_busy', { reason: 'other_user', takeover: false });
    const previous = this.operations.get(desktopId);
    if (previous && active.has(previous.status)) throw new HttpError(409, 'host_update_in_progress');
    if (this.operations.size >= 10000 && !previous) throw new HttpError(503, 'host_update_busy');
    const now = this.now();
    const operation = { id: randomUUID(), desktopId, actor, source, status: 'waiting', code: '', fromVersion: running, targetVersion: version,
      release: { version: release.version, filename: release.filename, size: release.size, sha256: release.sha256, path: release.url, ...(release.signature ? { signature: release.signature } : {}) },
      progress: null, rebootRequired: false, disconnectSessions, allowDowngrade: order < 0 && allowDowngrade, submittedAt: now, checkedAt: now, retryAt: now };
    this.operations.set(desktopId, operation); this.outcomes.delete(desktopId);
    this.audit('host_update_requested', actor, `${desktopId}:${running}>${version}:${source}${disconnectSessions ? ':disconnect' : ''}`);
    if (disconnectSessions) for (const session of [...r.sessions.values()]) if (session.desktopId === desktopId) r.stop(session, 'host_update');
    if (this.busy(desktopId)) { if (!disconnectSessions && this.sessionActive(desktopId)) operation.code = 'session_active'; }
    else this.dispatch(operation);
    this.changed();
    return this.view(desktopId, latest);
  }
  // A waiting request whose Host now runs the target (or a newer) version was
  // satisfied another way, e.g. the Host installer run inside the VM while
  // the request waited for its session to end. Dispatching it anyway made the
  // Host refuse it as same_version and the update look failed.
  resolvedWhileWaiting(operation, host) {
    const running = host?.identity.version;
    if (operation.status !== 'waiting' || typeof running !== 'string' || !versionPattern.test(running)) return false;
    const order = compareReleaseVersions(operation.targetVersion, running);
    if (order === 0) this.finish(operation, 'completed', 'result_unreported');
    else if (order < 0 && !operation.allowDowngrade) this.finish(operation, 'rejected', 'not_newer');
    else return false;
    return true;
  }
  dispatch(operation) {
    const r = this.relay, host = r.hosts.get(operation.desktopId), now = this.now();
    if (!this.authenticated(host, operation.desktopId) || !this.supported(host)) { this.finish(operation, 'unknown', 'host_disconnected'); return; }
    if (this.resolvedWhileWaiting(operation, host)) return;
    operation.status = 'dispatched'; operation.code = ''; operation.checkedAt = now;
    const { version, filename, size, sha256, path, signature } = operation.release;
    // Only a Host that announced release signatures may receive the field.
    const signed = host.releaseSignature === hostUpdateReleaseSignature && signature ? { signature } : {};
    if (!r.send(host.ws, { type: 'host.update.start', requestId: operation.id, version, filename, size, sha256, path, allowDowngrade: operation.allowDowngrade, disconnectSessions: operation.disconnectSessions, ...signed })) {
      r.dropHost(host, 'host_backpressure'); this.finish(operation, 'unknown', 'host_disconnected');
    }
  }
  finish(operation, status, code = '') {
    operation.status = status; operation.code = hostUpdateCodes.has(code) ? code : ''; operation.checkedAt = this.now();
    this.audit(auditEvents[status], operation.actor, `${operation.desktopId}:${operation.fromVersion}>${operation.targetVersion}:${operation.code || '-'}${operation.rebootRequired ? ':restart_required' : ''}`);
    this.remember(this.outcomes, operation.desktopId, status, operation.code, operation.fromVersion, operation.targetVersion, operation.checkedAt);
    this.changed();
  }
  remember(outcomes, desktopId, status, code, fromVersion, targetVersion, endedAt) {
    if (missingAttempt(status, code)) outcomes.set(desktopId, { status, code, fromVersion, targetVersion, endedAt });
    else outcomes.delete(desktopId);
  }
  // Operations live in memory only: after a broker restart the audit log
  // still says how each desktop's last attempt ended (a request without a
  // result was still under way, so it names nothing).
  restore() {
    const outcomes = new Map(), events = ['host_update_requested', ...Object.values(auditEvents)];
    try {
      const rows = this.relay.store.db.prepare(`SELECT event,subject,created_at FROM audit_events WHERE event IN (${events.map(() => '?').join(',')}) ORDER BY id DESC LIMIT 5000`).all(...events);
      for (const row of rows.reverse()) {
        if (row.event === 'host_update_requested') { const id = /^([A-Za-z0-9_-]{1,128}):/.exec(row.subject)?.[1]; if (id) outcomes.delete(id); continue; }
        const match = auditResult.exec(row.subject), code = match?.[4] === '-' ? '' : match?.[4];
        if (match && hostUpdateCodes.has(code) && Number.isSafeInteger(row.created_at)) this.remember(outcomes, match[1], auditStatus[row.event], code, match[2], match[3], row.created_at);
      }
    } catch { /* Without history nothing is named until the next attempt ends. */ }
    return outcomes;
  }
  // The desktop list's hostMissingAfterUpdate, or null. `desktop` is the
  // decorated list entry: its Proxmox state (VM state machine) or this
  // broker's PC power record says whether anything else explains a missing
  // Host. It names the attempt only; it never says why the Host is gone.
  missingAfterUpdate(desktop) {
    const id = desktop?.id, outcome = this.outcomes.get(id), operation = this.operations.get(id);
    if (!outcome || desktop.host?.connected !== false || (operation && active.has(operation.status))) return null;
    if (desktop.kind === 'vm') { if (desktop.powerState !== 'running' || !['host_lost', 'host_offline'].includes(desktop.vmState)) return null; }
    else if (desktop.kind !== 'pc' || this.relay.pcPower?.explainsAbsence(id, outcome.endedAt) !== false) return null;
    let lastSeenAt;
    try { lastSeenAt = this.relay.store.hostLastSeen(id); } catch { return null; }
    if (!hostMissingAfterUpdate({ now: this.now(), outcome, lastSeenAt, missingMs: this.missingMs, linkMs: this.linkMs })) return null;
    return { result: outcome.status, code: outcome.code, fromVersion: outcome.fromVersion, targetVersion: outcome.targetVersion };
  }
  message(host, value) {
    try {
      if (this.relay.hosts.get(host.identity.desktop_id) !== host) return;
      if (value.type === 'host.update.hello') this.hello(host, value);
      else if (value.type === 'host.update.status' && this.supported(host)) {
        this.report(host, value);
        // A final state is delivered: the Host may now clear its pending record.
        if (['completed', 'failed', 'rejected'].includes(value.state) && uuid.test(value.requestId ?? '')) this.relay.send(host.ws, { type: 'host.update.status.ack', requestId: value.requestId });
      }
    } catch { /* Optional maintenance reporting cannot break the control channel. */ }
  }
  hello(host, value) {
    if (value.protocol !== hostUpdateProtocol) return;
    host.updateProtocol = hostUpdateProtocol;
    host.releaseSignature = value.releaseSignature === hostUpdateReleaseSignature ? hostUpdateReleaseSignature : undefined;
    if (typeof value.version === 'string' && versionPattern.test(value.version)) {
      host.identity.version = value.version;
      try { this.relay.store.hostSeen(host.identity.id, this.now(), value.version); } catch { /* Version display is optional. */ }
    }
    const result = value.result && typeof value.result === 'object' && !Array.isArray(value.result) && uuid.test(value.result.requestId ?? '') ? value.result : null;
    if (result) this.report(host, result);
    this.relay.send(host.ws, { type: 'host.update.hello.ack', protocol: hostUpdateProtocol, releaseSignature: hostUpdateReleaseSignature, ...(result ? { result: result.requestId } : {}) });
    // A Host that returned without a result: only the observed target
    // version is evidence of completion; anything else stays unconfirmed.
    const operation = this.operations.get(host.identity.desktop_id);
    if (!result && operation?.status === 'installing') {
      if (host.identity.version === operation.targetVersion) this.finish(operation, 'completed', 'result_unreported');
      else this.finish(operation, 'unknown', 'install_result_missing');
    } else if (operation) this.resolvedWhileWaiting(operation, host);
    this.changed();
  }
  report(host, value) {
    const operation = this.operations.get(host.identity.desktop_id);
    if (!operation || operation.id !== value.requestId || !reportable.has(operation.status)) return;
    const code = value.code ?? '';
    if (!hostStates.has(value.state) || !hostUpdateCodes.has(code) || (value.rebootRequired !== undefined && typeof value.rebootRequired !== 'boolean') ||
      (value.received !== undefined && !integer(value.received, operation.release.size)) || (value.total !== undefined && value.total !== operation.release.size) ||
      (value.version !== undefined && (typeof value.version !== 'string' || !versionPattern.test(value.version)))) return;
    const now = this.now();
    if (value.version !== undefined) host.identity.version = value.version;
    if (typeof value.rebootRequired === 'boolean') operation.rebootRequired = value.rebootRequired;
    switch (value.state) {
      case 'accepted': case 'downloading': case 'verifying': case 'installing':
        operation.status = value.state; operation.code = code; operation.checkedAt = now;
        operation.progress = value.state === 'downloading' && value.received !== undefined ? Math.floor(value.received * 100 / operation.release.size) : value.state === 'downloading' ? 0 : value.state === 'accepted' ? null : 100;
        this.changed(); return;
      case 'deferred':
        operation.status = 'waiting'; operation.code = 'session_active'; operation.checkedAt = now; operation.retryAt = now + this.retryMs; operation.progress = null;
        this.changed(); return;
      case 'completed':
        // Completion means the reporting Host now runs the requested version.
        if (value.version !== operation.targetVersion) { this.finish(operation, 'failed', code || 'install_failed'); return; }
        operation.progress = 100; this.finish(operation, 'completed', code); return;
      default:
        operation.progress = null; this.finish(operation, value.state === 'rejected' ? 'rejected' : 'failed', code || 'install_failed');
    }
  }
  tick() {
    const now = this.now();
    for (const operation of this.operations.values()) {
      if (operation.status === 'waiting') {
        if (now - operation.submittedAt > this.waitMs) this.finish(operation, 'expired', 'wait_expired');
        else if (now >= operation.retryAt && !this.busy(operation.desktopId) && this.supported(this.relay.hosts.get(operation.desktopId))) { this.dispatch(operation); this.changed(); }
      } else if (operation.status === 'dispatched' && now - operation.checkedAt > this.dispatchMs) this.finish(operation, 'unknown', 'host_no_response');
      else if (['accepted', 'downloading', 'verifying'].includes(operation.status) && now - operation.checkedAt > this.progressMs) this.finish(operation, 'unknown', 'host_no_progress');
      else if (operation.status === 'installing' && now - operation.checkedAt > this.installMs) this.finish(operation, 'unknown', 'host_not_returned');
    }
  }
  audit(event, actor, detail) { try { this.relay.store.audit(event, actor, detail); } catch { /* Audit I/O cannot fail an update state change. */ } }
}
