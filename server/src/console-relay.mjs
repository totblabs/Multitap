import { randomUUID, createHash, timingSafeEqual } from 'node:crypto';
import { WebSocketServer } from 'ws';
import { HttpError } from './errors.mjs';
import { newToken, tokenHash } from './security.mjs';
import { ConsoleRfb, ConsoleWriteQueue, ConsoleProtocolError } from './console-rfb.mjs';
import { ConsoleReadBudget, consoleReadLimits } from './console-read-budget.mjs';
import { relocatableErrors } from './vm-relocate.mjs';

export const consoleRelayProtocol = 'rfb-none-v1';
const uuid = value => typeof value === 'string' && /^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/i.test(value);
const machineUuid = value => uuid(value) && !/^(?:0{32}|f{32})$/i.test(value.replaceAll('-', ''));
const stamp = value => createHash('sha256').update(JSON.stringify(value)).digest('hex');
const equal = (a, b) => typeof a === 'string' && typeof b === 'string' && a.length === b.length && timingSafeEqual(Buffer.from(a), Buffer.from(b));
const auditEndCodes = new Set(['authorization_revoked', 'server_stopping', 'console_taken_over', 'console_auth_failed', 'console_backpressure', 'console_busy', 'console_cancelled', 'console_certificate_error', 'console_clipboard_denied', 'console_crypto_unavailable', 'console_identity_changed', 'console_identity_unavailable', 'console_invalid_upstream', 'console_limit_reached', 'console_limits_exceeded', 'console_permission_denied', 'console_protocol_error', 'console_rfb_unsupported', 'console_timeout', 'console_transport_closed', 'console_unavailable', 'console_vm_unavailable', 'console_no_display']);
for (const reason of ['inactive', 'state_missing', 'state_invalid', 'identity_changed', 'foreign_changed', 'unavailable', 'cancelled', 'busy', 'timeout']) auditEndCodes.add('console_display_' + reason);
const auditLimitScopes = new Set(['input_rate', 'identity_buffer', 'rfb_parser', 'output_queue']);
const auditEndPhases = new Set(['preparing', 'issued', 'auth', 'opening', 'rfb', 'active']);
const auditEndTriggers = new Set(['grant_deadline', 'auth_deadline', 'handshake_deadline', 'session_deadline', 'pong_deadline', 'identity_check', 'client_close', 'client_error', 'upstream_close', 'upstream_error']);
function code(error) {
  if (error instanceof ConsoleProtocolError) return error.code;
  if (error?.code === 'proxmox_permission_denied') return 'console_permission_denied';
  if (error?.code === 'proxmox_certificate_error') return 'console_certificate_error';
  if (error instanceof HttpError && /^console_[a-z_]+$/.test(error.code)) return error.code;
  if (error?.code === 'desktop_not_found' || error?.code === 'session_expired') return 'authorization_revoked';
  return 'console_unavailable';
}

export class ConsoleRelay {
  constructor(relay, { now = Date.now, grantMs = 30000, authMs = 5000, handshakeMs = 10000, identityMs = 5000, readSetTimer = setTimeout, readClearTimer = clearTimeout } = {}) {
    this.r = relay; this.now = now; this.grantMs = grantMs; this.authMs = authMs; this.handshakeMs = handshakeMs; this.identityMs = identityMs;
    this.readSetTimer = readSetTimer; this.readClearTimer = readClearTimer;
    this.grants = new Map(); this.attempts = new Map(); this.closed = false;
    this.wss = new WebSocketServer({ noServer: true, maxPayload: 1024 * 1024, perMessageDeflate: false, maxFragments: 256, closeTimeout: 1000 });
  }
  blocked(desktopId, except) { return [...this.grants.values()].some(value => value.desktopId === desktopId && value.id !== except); }
  audit(event, value, reason, limitScope, diagnostic) {
    // Correlate access only: never serialize the grant/config, credentials,
    // ticket, upstream address, desktop contents or arbitrary error text.
    const subject = { desktopId: value.desktopId, consoleSessionId: value.id };
    if (reason !== undefined) subject.code = auditEndCodes.has(reason) ? reason : 'console_unavailable';
    // Whether the console-only display (virtual outputs off, the QEMU display
    // on) was asked of the Host, or why not; fixed codes only (without it
    // nothing shows whether it ran).
    if (event === 'console_granted' || event === 'console_ready') {
      const status = value.qemuOperation?.status, model = value.snapshot?.model;
      subject.qemu = value.qemuOperation ? (typeof status === 'string' && /^[a-z_]{1,32}$/.test(status) ? status : 'requested')
        : !this.r.qemuConsole?.supported(this.r.hosts.get(value.desktopId)) ? 'host_unsupported'
        : model === 'std' ? 'not_requested' : 'model_' + (['virtio', 'qxl', 'cirrus', 'vmware', 'none', 'serial'].includes(model) ? model : 'other');
      // The page was told the sign-in screen may be on another display (signinElsewhere).
      if (value.consoleHint === 'signin_other_display') subject.hint = value.consoleHint;
    }
    if (event === 'console_ended' && ['console_limits_exceeded', 'console_backpressure'].includes(subject.code) && auditLimitScopes.has(limitScope)) subject.limitScope = limitScope;
    if (event === 'console_ended' && auditEndTriggers.has(diagnostic?.trigger)) {
      subject.trigger = diagnostic.trigger;
      if (auditEndPhases.has(value.state)) subject.phase = value.state;
      if ((diagnostic.trigger === 'client_close' || diagnostic.trigger === 'upstream_close') &&
          Number.isInteger(diagnostic.transportCode) && diagnostic.transportCode >= 1000 && diagnostic.transportCode <= 4999) subject.transportCode = diagnostic.transportCode;
    }
    try { this.r.store.audit(event, value.userId, JSON.stringify(subject), this.now()); }
    catch { /* Audit I/O cannot retain an owner or change its authorization. */ }
  }
  authorize(value, readOnly = false) {
    const user = this.r.consoleAuthenticate ? this.r.consoleAuthenticate(value.tokenHash, value.desktopId, value.origin, readOnly) : this.r.store.authenticate(value.tokenHash);
    const desktop = user && user.id === value.userId && !user.must_change_credentials && this.r.store.assignedDesktop(user.id, value.desktopId);
    if (!desktop || desktop.kind !== 'vm') throw new HttpError(404, 'desktop_not_found');
    if (desktop.pve_node !== value.node || desktop.pve_vmid !== value.vmid || desktop.pve_machine_uuid?.toLowerCase() !== value.machineUuid) throw new HttpError(409, 'console_identity_changed');
    if (value.profile === 'novnc') {
      const current = this.r.store.clipboardPolicy(value.userId, value.desktopId);
      if (['textToRemote', 'textToLocal'].some(key => value.clipboardPolicy[key] && current[key] !== true)) throw new HttpError(403, 'console_clipboard_denied');
    }
    return desktop;
  }
  idle(value) {
    if (this.r.powerBusy?.(value.desktopId) || [...this.r.sessions.values()].some(session => session.desktopId === value.desktopId) || this.blocked(value.desktopId, value.id)) throw new HttpError(409, 'console_busy');
    // QEMU-display preparation does not replace capture cleanup. Keep the
    // existing strict stop/capability barrier; only this grant's exact QEMU
    // reservation may pass its own display mutation lock.
    const reservation = this.r.qemuConsole?.owns(value) ? value.qemuOperation : undefined;
    try { this.r.assertDisplayIdle(value.desktopId, 'stream', value.id, reservation); }
    catch { throw new HttpError(409, 'console_busy'); }
  }
  current(value) {
    if (this.closed || value.closed || this.grants.get(value.id) !== value) throw new HttpError(409, 'console_cancelled');
    this.authorize(value); this.idle(value);
    if (stamp(this.r.config()) !== value.configStamp) throw new HttpError(409, 'console_identity_changed');
  }
  async snapshot(value) {
    this.current(value);
    let fresh;
    try { fresh = await this.r.proxmox.consoleSnapshot(value.config, value.vmid, { node: value.node, expectedUuid: value.machineUuid, signal: value.abort.signal }); }
    catch (error) {
      // Before the first snapshot only: the VM may have moved (migration, HA,
      // restore to a new VM ID). Found again by its UUID, the grant follows
      // the desktop row to the new place and reads it there (vm-relocate.mjs).
      if (value.snapshot || !this.r.vmRelocation || !relocatableErrors.has(error?.code)) throw error;
      this.current(value);
      const moved = await this.r.vmRelocation.relocateDesktop(value.config, value.desktopId, { node: value.node, vmid: value.vmid }, error.code, { signal: value.abort.signal, waitMs: 10000 });
      if (!moved) throw error;
      value.node = moved.node; value.vmid = moved.vmid;
      this.current(value);
      fresh = await this.r.proxmox.consoleSnapshot(value.config, value.vmid, { node: value.node, expectedUuid: value.machineUuid, signal: value.abort.signal });
    }
    this.current(value);
    if (fresh.machineUuid !== value.machineUuid || fresh.vmid !== value.vmid || (value.snapshot && (fresh.node !== value.snapshot.node || fresh.model !== value.snapshot.model))) throw new HttpError(409, 'console_identity_changed');
    value.snapshot = fresh; value.checkedAt = this.now();
  }
  // vm-config-sweep item 5: nobody signed in and unlocked (signed out, locked)
  // or a security prompt open, so the Host can only park its virtual display
  // for the Console (qemu_console_route.hpp), and Proxmox lists a passed-through
  // PCI device. A GPU or vGPU among them keeps its own display, which Windows
  // may keep primary: the sign-in screen is then drawn where the Console
  // cannot show it. Neither fact proves that (a standard user's session parks
  // too and is not visible here; the device may not be a GPU), so the page
  // only says it may be so. The Host's capabilities must be current.
  signinElsewhere(value) {
    const host = this.r.hosts.get(value.desktopId), caps = host?.capabilities;
    if (value.snapshot?.passthrough !== true || !host || host.ws?.readyState !== 1 || host.capabilitiesPending || !caps || caps.service !== true) return false;
    if (Date.now() - (host.capabilitiesAt ?? 0) > this.r.heartbeatMs * 3) return false;
    return caps.interactiveSession === false || caps.secureDesktop === true;
  }
  async create(user, hash, desktopId, { signal, origin = this.r.publicOrigin, profile = 'native' } = {}) {
    this.r.assertAccessAdmission?.();
    if (!['native', 'novnc'].includes(profile)) throw new HttpError(400, 'invalid_console_profile');
    if (this.closed) throw new HttpError(503, 'console_unavailable');
    const desktop = this.r.store.assignedDesktop(user.id, desktopId);
    if (!desktop || desktop.kind !== 'vm') throw new HttpError(404, 'desktop_not_found');
    if (!Number.isSafeInteger(desktop.pve_vmid) || desktop.pve_vmid < 100 || !machineUuid(desktop.pve_machine_uuid)) throw new HttpError(409, 'console_identity_unavailable');
    const now = this.now();
    for (const [id, entry] of this.attempts) if (now - entry.started >= 60000) this.attempts.delete(id);
    const bucket = this.attempts.get(user.id);
    if ((!bucket && this.attempts.size >= 256) || (bucket && bucket.count >= 6)) throw new HttpError(429, 'console_limit_reached');
    if (this.grants.size >= 8 || [...this.grants.values()].filter(value => value.userId === user.id).length >= 2) throw new HttpError(429, 'console_limit_reached');
    const ticket = newToken(), config = this.r.config();
    const policy = profile === 'novnc' ? this.r.store.clipboardPolicy(user.id, desktopId) : null;
    const clipboardPolicy = profile === 'novnc' ? { textToRemote: policy?.textToRemote === true, textToLocal: policy?.textToLocal === true } : null;
    const value = { id: randomUUID(), userId: user.id, tokenHash: hash, desktopId, origin, profile, clipboardPolicy, node: desktop.pve_node, vmid: desktop.pve_vmid,
      machineUuid: desktop.pve_machine_uuid.toLowerCase(), config, configStamp: stamp(config), abort: new AbortController(),
      ticketHash: tokenHash(ticket), state: 'preparing', expiresAt: now + this.grantMs, pending: 1, closed: false };
    this.authorize(value); this.idle(value);
    // Counted only once the desktop is free: a refusal above (console_busy while a stream ends) does no Proxmox
    // work, and the page asks again every second until the stream's cleanup is over ('스트리밍 끊고 연결').
    if (!bucket) this.attempts.set(user.id, { started: now, count: 1 }); else ++bucket.count;
    this.grants.set(value.id, value); this.r.activityAt = Date.now();
    const abort = () => this.retire(value, 'console_cancelled');
    signal?.addEventListener('abort', abort, { once: true });
    try {
      if (signal?.aborted) throw new HttpError(409, 'console_cancelled');
      await this.snapshot(value); this.current(value);
      // Read before the preparation, which withdraws the Host's capabilities.
      if (profile === 'novnc' && this.signinElsewhere(value)) value.consoleHint = 'signin_other_display';
      const qemuConsolePreparation = await this.r.qemuConsole?.prepare(value);
      this.current(value);
      if (qemuConsolePreparation) await this.snapshot(value);
      value.state = 'issued'; value.expiresAt = this.now() + this.grantMs;
      this.audit('console_granted', value);
      return { id: value.id, ticket, relayPath: '/relay/console/' + value.id, expiresAt: new Date(value.expiresAt).toISOString(), ...(profile === 'novnc' ? { clipboardPolicy: { ...clipboardPolicy } } : {}), ...(qemuConsolePreparation ? { qemuConsolePreparation: this.r.qemuConsole.preparation(value) } : {}), ...(value.consoleHint ? { consoleHint: value.consoleHint } : {}) };
    } catch (error) {
      this.retire(value, code(error)); throw new HttpError(code(error) === 'console_permission_denied' ? 403 : 409, code(error));
    } finally { signal?.removeEventListener('abort', abort); --value.pending; this.collect(value); }
  }
  // Ends every console connection on this desktop for an assigned account's
  // stream ('Console 끊고 연결'). Returns how many were ended.
  release(user, desktopId) {
    const values = [...this.grants.values()].filter(value => value.desktopId === desktopId && !value.closed);
    for (const value of values) this.retire(value, 'console_taken_over');
    if (values.length) try { this.r.store.audit('console_taken_over', user.id, JSON.stringify({ desktopId, consoles: values.length, otherUser: values.some(value => value.userId !== user.id) })); } catch { /* Optional audit. */ }
    return values.length;
  }
  cancel(user, hash, desktopId, id) {
    const value = this.grants.get(id);
    if (!value || value.userId !== user.id || value.tokenHash !== hash || value.desktopId !== desktopId) throw new HttpError(404, 'console_session_not_found');
    this.retire(value, 'console_cancelled'); return { id, released: true };
  }
  // The live browser grant of exactly this principal, VM and origin, or 404.
  browserGrant(user, hash, desktopId, id, origin) {
    const value = this.grants.get(id), now = this.now();
    if (!value || value.closed || value.profile !== 'novnc' || value.userId !== user.id ||
        value.tokenHash !== hash || value.desktopId !== desktopId || value.origin !== origin) throw new HttpError(404, 'console_session_not_found');
    this.authorize(value, true);
    const deadline = ['preparing', 'issued'].includes(value.state) ? value.expiresAt : value.state === 'active' ? value.sessionDeadline : value.deadline;
    if (!Number.isFinite(deadline) || now >= deadline) throw new HttpError(404, 'console_session_not_found');
    return value;
  }
  preparationStatus(user, hash, desktopId, id, origin) {
    const value = this.browserGrant(user, hash, desktopId, id, origin);
    // Read only: never renew a login/hold, contact PVE/Host, or retire a grant.
    return { qemuConsolePreparation: this.r.qemuConsole.preparation(value) };
  }
  // ime-state-v1 (ime-state.mjs): the VM's 한/영 mode, asked of the Host now.
  // Only for a grant past its preparation; the answer is given
  // only while that grant is still the same live one. {} means unknown.
  async imeStatus(user, hash, desktopId, id, origin) {
    const value = this.browserGrant(user, hash, desktopId, id, origin);
    if (value.state === 'preparing') return {};
    const korean = await this.r.ime.query(desktopId);
    if (this.browserGrant(user, hash, desktopId, id, origin) !== value || typeof korean !== 'boolean') return {};
    return { imeKorean: korean };
  }
  upgrade(req, socket, head, url, reject) {
    if (!url.pathname.startsWith('/relay/console/')) return false;
    if (url.search || url.hash || (req.headers.origin && req.headers.origin !== (req.brokerOrigin ?? this.r.publicOrigin))) { reject(403, 'Forbidden'); return true; }
    const id = url.pathname.slice('/relay/console/'.length), token = /^Bearer ([A-Za-z0-9_-]{43})$/.exec(req.headers.authorization ?? '')?.[1];
    const value = this.grants.get(id);
    let spent = false;
    try {
      if (!uuid(id) || !token || !value || value.profile !== 'native' || value.tokenHash !== tokenHash(token) || (req.brokerOrigin && req.brokerOrigin !== value.origin) || value.state !== 'issued' || value.expiresAt <= this.now()) throw new Error();
      this.current(value);
      value.state = 'auth'; value.deadline = this.now() + this.authMs; spent = true; // Spend only this authenticated grant.
      this.wss.handleUpgrade(req, socket, head, ws => this.attach(value, ws));
    } catch { if (spent) this.retire(value, 'console_auth_failed'); reject(401, 'Unauthorized'); }
    return true;
  }
  // The HTTP owner supplies only a freshly authenticated cookie identity and
  // validated HTTPS Origin. Browser code never receives a bearer or VNC ticket.
  upgradeBrowser(req, socket, head, id, reject, { user, hash, origin } = {}) {
    if ((typeof req.url === 'string' && /[?#]/.test(req.url)) || typeof origin !== 'string' || !origin.startsWith('https://') || req.headers.origin !== origin || (req.brokerOrigin && req.brokerOrigin !== origin)) { reject(403, 'Forbidden'); return true; }
    const value = this.grants.get(id); let spent = false;
    try {
      if (!uuid(id) || !value || value.profile !== 'novnc' || !user || user.id !== value.userId ||
          !equal(hash, value.tokenHash) || origin !== value.origin || value.state !== 'issued' || value.expiresAt <= this.now() || !value.ticketHash) throw new Error();
      this.current(value);
      value.ticketHash = null; value.state = 'opening'; value.deadline = this.now() + this.handshakeMs; spent = true;
      this.wss.handleUpgrade(req, socket, head, ws => { this.attach(value, ws); void this.open(value); });
    } catch { if (spent) this.retire(value, 'console_auth_failed'); reject(401, 'Unauthorized'); }
    return true;
  }
  attach(value, socket) {
    value.client = socket; value.clientQueue = new ConsoleWriteQueue(socket,
      reason => this.retire(value, reason, reason === 'console_backpressure' ? 'output_queue' : undefined),
      { onWritable: () => {
        try { this.current(value); value.readBudget?.drain(); }
        catch (error) { this.retire(value, code(error)); }
      }, setTimer: this.readSetTimer, clearTimer: this.readClearTimer });
    socket.on('error', () => this.retire(value, 'console_transport_closed', undefined, { trigger: 'client_error' }));
    socket.on('close', transportCode => { clearTimeout(value.endTimer); this.retire(value, 'console_transport_closed', undefined, { trigger: 'client_close', transportCode }); this.collect(value); });
    if (value.closed) { this.retire(value, 'console_cancelled'); return; }
    socket.on('pong', () => { value.pongAt = this.now(); });
    socket.on('message', (data, binary) => {
      try {
        this.current(value);
        const now = this.now(); if (!value.rateAt || now - value.rateAt >= 1000) { value.rateAt = now; value.messages = value.inputBytes = 0; }
        if (++value.messages > 1000 || (value.inputBytes += data.length) > 1024 * 1024) { this.retire(value, 'console_limits_exceeded', 'input_rate'); return; }
        if (value.state === 'auth') {
          if (binary || data.length > 512 || value.deadline <= now) throw new ConsoleProtocolError('console_auth_failed');
          const input = JSON.parse(data.toString());
          if (!input || Object.keys(input).sort().join('|') !== 'ticket|type' || input.type !== 'console-auth' || typeof input.ticket !== 'string' || !/^[A-Za-z0-9_-]{43}$/.test(input.ticket) || !equal(tokenHash(input.ticket), value.ticketHash)) throw new ConsoleProtocolError('console_auth_failed');
          value.ticketHash = null; value.state = 'opening'; value.deadline = now + this.handshakeMs;
          void this.open(value);
        } else if (!binary || !value.rfb) throw new ConsoleProtocolError();
        else {
          try { value.rfb.downstream(data); }
          catch (error) { this.retire(value, code(error), code(error) === 'console_limits_exceeded' ? 'rfb_parser' : undefined); return; }
          if (value.rfb.clientReady && value.state !== 'active') { value.state = 'active'; value.sessionDeadline = now + 3600000; value.pongAt = now; }
        }
      } catch (error) { this.retire(value, code(error)); }
    });
  }
  async open(value) {
    ++value.pending;
    try {
      await this.snapshot(value);
      const connection = await this.r.proxmox.openConsole(value.config, value.snapshot, { signal: value.abort.signal });
      value.upstream = connection.socket;
      value.upstream.on('close', transportCode => { this.retire(value, 'console_transport_closed', undefined, { trigger: 'upstream_close', transportCode }); this.collect(value); });
      value.upstream.on('error', () => this.retire(value, 'console_transport_closed', undefined, { trigger: 'upstream_error' }));
      value.upstream.on('unexpected-response', (_request, response) => {
        const status = response.statusCode; response.destroy();
        this.retire(value, status === 401 || status === 403 ? 'console_permission_denied' : 'console_unavailable');
      });
      this.current(value);
      if (value.upstream.readyState === 2 || value.upstream.readyState === 3) throw new ConsoleProtocolError('console_transport_closed');
      value.upstreamQueue = new ConsoleWriteQueue(value.upstream, reason => this.retire(value, reason, reason === 'console_backpressure' ? 'output_queue' : undefined), { limit: 1024 * 1024 });
      let verifiedAfterDial = false, bufferedBytes = 0;
      const buffered = [];
      value.rfb = new ConsoleRfb({ password: connection.password, qemuKeyboard: true, profile: value.profile, clipboardPolicy: value.clipboardPolicy,
        acceptedInput: () => this.r.consoleInputAccepted?.(value) !== false,
        upstream: bytes => value.upstreamQueue.push(bytes), downstream: bytes => value.clientQueue.push(bytes),
        ready: () => { this.current(value); value.state = 'rfb'; value.deadline = this.now() + this.handshakeMs;
          // Upstream RFB ServerInit is verified; client pixels/Windows login
          // and eventual transport cleanup are separate observations.
          if (!value.readyAudited) { value.readyAudited = true; this.audit('console_ready', value); }
          if (value.profile !== 'novnc') value.clientQueue.push(JSON.stringify({ type: 'console-ready', protocol: consoleRelayProtocol, keyboardProtocol: 'qemu-keyboard-v1' }), false); },
      });
      const deliver = data => {
        try {
          // Every delayed/chunked delivery revalidates the exact grant and
          // current account/VM. A credit timer conveys no authorization.
          this.current(value);
          value.rfb.upstream(data);
          if (!value.closed && value.rfb.clientReady && value.state !== 'active') { value.state = 'active'; value.sessionDeadline = this.now() + 3600000; value.pongAt = this.now(); }
        } catch (error) { this.retire(value, code(error), code(error) === 'console_limits_exceeded' ? 'rfb_parser' : undefined); }
      };
      value.readBudget = new ConsoleReadBudget(value.upstream, deliver,
        reason => this.retire(value, reason, reason === 'console_limits_exceeded' ? 'output_queue' : undefined),
        { now: this.now, setTimer: this.readSetTimer, clearTimer: this.readClearTimer,
          // One read is at most64KiB. Reserve another chunk for the bounded
          // readiness envelope/ServerInit replacement and rectangle headers.
          canConsume: () => value.clientQueue.hasCapacity(2 * consoleReadLimits.chunk, 2) });
      const consume = (data, binary) => {
        try {
          this.current(value); if (!binary) throw new ConsoleProtocolError();
          if (!data.length) return;
          if (!verifiedAfterDial) {
            if (buffered.length >= 256 || (bufferedBytes += data.length) > 65536) { this.retire(value, 'console_limits_exceeded', 'identity_buffer'); return; }
            buffered.push(Buffer.from(data)); return;
          }
          value.readBudget.push(data);
        } catch (error) { this.retire(value, code(error)); }
      };
      value.upstream.on('message', consume);
      // The opened VNC endpoint must still identify the same VM before any
      // upstream authentication/readiness or desktop byte reaches the client.
      await this.snapshot(value); this.current(value); verifiedAfterDial = true;
      for (const bytes of buffered) consume(bytes, true);
      buffered.length = 0;
    } catch (error) { this.retire(value, code(error)); }
    finally { --value.pending; this.collect(value); }
  }
  retire(value, reason, limitScope, diagnostic) {
    const first = !value.closed;
    if (first) { value.closed = true; value.ticketHash = null; value.config = null;
      this.r.qemuConsole?.cancelGrant(value);
      // Admission is retired exactly once. collect() still waits for actual
      // async/transport completion; this row does not acknowledge that cleanup.
      this.audit('console_ended', value, reason, limitScope, diagnostic);
    }
    value.readBudget?.close(); value.rfb?.close(); value.clientQueue?.close(); value.upstreamQueue?.close();
    if (!value.clientEnding && value.client?.readyState === 1) {
      value.clientEnding = true;
      value.endTimer = setTimeout(() => value.client?.terminate(), 1000); value.endTimer.unref?.();
      if (value.profile === 'novnc') {
        try { value.client.close(1008, auditEndCodes.has(reason) ? reason : 'console_unavailable'); } catch { value.client.terminate(); }
      } else try { value.client.send(JSON.stringify({ type: 'console-ended', code: reason }), { binary: false }, error => {
        try { if (error) value.client.terminate(); else value.client.close(1008, reason); } catch { value.client.terminate(); }
      }); } catch { value.client.terminate(); }
    } else if (!value.clientEnding) { try { value.client?.terminate(); } catch {} }
    // Install client retirement before abort: a synchronous upstream-close
    // callback must not discard the fixed end reason queued for the client.
    value.abort.abort();
    // Includes transports returned late by an opener after cancellation.
    try { if (value.upstream && value.upstream.readyState !== 3) value.upstream.terminate(); } catch {}
    this.collect(value);
  }
  collect(value) {
    // An observer cancel cannot release the slot while the actual opener,
    // fresh identity query or owned transport still exists.
    if (!value.closed || value.pending || [value.client, value.upstream].some(socket => socket && socket.readyState !== 3)) return;
    if (this.grants.get(value.id) === value) { this.grants.delete(value.id); this.r.activityAt = Date.now(); }
  }
  revalidate() {
    const now = this.now();
    for (const value of this.grants.values()) {
      if (value.closed) { this.collect(value); continue; }
      try {
        this.current(value);
        const grant = value.state === 'issued' || value.state === 'preparing';
        if (grant ? now >= value.expiresAt : value.state !== 'active' ? now >= value.deadline : now >= value.sessionDeadline) {
          const trigger = grant ? 'grant_deadline' : value.state === 'active' ? 'session_deadline' : value.state === 'auth' ? 'auth_deadline' : 'handshake_deadline';
          this.retire(value, 'console_timeout', undefined, { trigger }); continue;
        }
        if (value.state === 'active') {
          if (now - value.pongAt > 30000) { this.retire(value, 'console_timeout', undefined, { trigger: 'pong_deadline' }); continue; }
          if (!value.pingAt || now - value.pingAt > 10000) { value.pingAt = now; value.client.ping(); }
        }
        if (!value.checking && !value.pending && now - value.checkedAt >= this.identityMs) {
          value.checking = true; ++value.pending;
          void this.snapshot(value).catch(error => this.retire(value, code(error), undefined, { trigger: 'identity_check' })).finally(() => { value.checking = false; --value.pending; this.collect(value); });
        }
      } catch (error) { this.retire(value, code(error)); }
    }
  }
  close() { this.closed = true; for (const value of this.grants.values()) this.retire(value, 'server_stopping'); this.wss.close(); }
}
