// The broker's one explicit VM state machine (overview: ARCHITECTURE.md, "VM state").
//
// Signals:   (1) power tasks the broker issued, (2) Proxmox VM status,
//            (3) QEMU guest agent ping (optional, privilege-gated),
//            (4) Host connection events (connected / ready / disconnected /
//                session), which arrive instantly over the Host channel.
// Output:    { state, reason, revision } per VM. `state` is what a tile shows:
//   off              꺼짐
//   starting         켜는 중            start task in flight (or just done, Proxmox not yet running)
//   windows_starting Windows 시작 중    Proxmox running, agent not answering, Host not connected
//   preparing        Host connected, Windows login / virtual display / session coming up (grace window)
//   host_wait        연결 준비 중       agent answers / Host connected but not ready yet
//   ready            연결 가능          Host connected and ready
//   in_use           사용 중            a session is active
//   stopping         종료 중            shutdown / stop in flight or not yet seen as stopped
//   rebooting        다시 시작 중       reboot task in flight
//   host_lost        Host 연결 끊김     the Host dropped, VM healthy (agent answers)
//   host_offline     Host 연결 안 됨    running, no Host, nothing says it is booting
//   unknown          확인 중            Proxmox status unavailable
// `reason` is a fixed code (never free text). `revision` only grows, also
// across broker restarts (milliseconds), so a client ignores an older answer.
//
// The pure function `deriveVmState` decides; `VmStates` collects the signals:
// events set per-VM memory, and a bounded per-VM watch (Proxmox status of that
// one VM every 1-5 s, at most `watchMs`) runs only while a transition is open:
// after a power task, and after a Host drop that nothing explains.

const SCHEDULE = [1000, 1000, 1500, 2000, 2000, 3000, 5000];

// What the Host says is still missing before it can stream, as the clients
// read it (same precedence as the Windows/Android clients: the console
// preparation reason while no interactive session, else the viewport failure
// reason). Returns null (nothing missing / nothing reported),
//   { transient: 'windows_login' | 'display_preparing' | 'session_preparing' }
//   -- a normal step right after the Host connected (auto-logon, virtual display coming up), or
//   { transient: null }  -- a definite failure or a state that needs a person
//   (driver missing/untrusted, administrator required, reboot required, recovery,
//   conflicts, locked or RDP-held session, anything this broker does not know).
const LOGIN_CODES = new Set(['windows_login_required']);
const SESSION_CODES = new Set(['no_interactive_session', 'windows_session_unknown', 'viewport_probe_pending', 'viewport_probe_failed', 'viewport_probe_timeout', 'viewport_support_unavailable', 'viewport_connector_identity_unavailable', 'viewport_connector_identity_ambiguous', 'viewport_ccd_query_failed', 'viewport_ccd_source_invalid', 'viewport_ccd_snapshot_limit', 'viewport_topology_changed', 'viewport_ccd_validate_failed', 'mode_catalog_failed', 'mode_catalog_timeout', 'mode_catalog_cancelled', 'mode_catalog_registration_timeout', 'mode_catalog_not_verified', 'owned_device_restart_failed', 'owned_device_restart_status_unavailable']);
const DISPLAY_CODES = new Set(['owned_virtual_output_inactive', 'owned_virtual_display_not_ready', 'native_display_not_ready', 'native_topology_not_ready', 'native_display_change_pending', 'viewport_sources_unavailable', 'monitor_not_available', 'host_maintenance', 'viewport_busy']);
export function classifyHostPreparation(desktop) {
  const host = desktop?.host;
  if (!host?.connected) return null;
  const caps = host.capabilities;
  if (host.capabilitiesPending) return { transient: 'session_preparing' };
  if (!caps) return null; // a legacy Host reports nothing to classify
  const consolePrep = desktop.consolePreparation;
  const code = consolePrep?.supported === true && caps.interactiveSession !== true ? consolePrep.reason : caps.viewportFailureReason;
  if (typeof code === 'string' && code !== '') {
    if (LOGIN_CODES.has(code)) return { transient: 'windows_login' };
    if (DISPLAY_CODES.has(code)) return { transient: 'display_preparing' };
    if (SESSION_CODES.has(code)) return { transient: 'session_preparing' };
    return { transient: null };
  }
  if (caps.interactiveSession === false) return { transient: 'windows_login' };
  if (caps.virtualDisplayReady === false) return { transient: 'display_preparing' };
  return { transient: 'session_preparing' };
}

export function deriveVmState({ now, power, powerStale, powerError, task, uptimeSeconds, host, ready, session, agent, droppedAt, reconnected = false, bootMs = 300000, settleMs = 20000, hostConnectedAt = null, preparation = null, preparingMs = 180000 }) {
  if (powerError || powerStale || power === 'unknown') return { state: 'unknown', reason: 'power_status_unavailable' };
  const { active, last } = task;
  const lastOk = last && last.status === 'stopped' && last.exit_status === 'OK' && last.checked_at !== null ? last : null;
  const recent = (limit) => lastOk && now - lastOk.checked_at < limit;
  const failed = last && last.status === 'stopped' && last.exit_status !== 'OK' && last.checked_at !== null && now - last.checked_at < 60000;
  if (active) {
    if (active.action === 'start') return { state: 'starting', reason: 'power_task_start' };
    if (active.action === 'reboot') return { state: 'rebooting', reason: 'power_task_reboot' };
    return { state: 'stopping', reason: `power_task_${active.action}` };
  }
  if (power === 'stopped') {
    // The start finished but the status read still says stopped.
    if (recent(settleMs) && lastOk.action === 'start') return { state: 'starting', reason: 'awaiting_proxmox' };
    return { state: 'off', reason: failed ? 'power_task_failed' : 'proxmox_stopped' };
  }
  // power === 'running'
  if (recent(settleMs) && (lastOk.action === 'shutdown' || lastOk.action === 'stop')) return { state: 'stopping', reason: 'awaiting_proxmox' };
  // A reboot just acknowledged leaves the old Host connected until Windows drops it.
  if (host && !reconnected && recent(settleMs) && lastOk.action === 'reboot') return { state: 'rebooting', reason: 'awaiting_host_drop' };
  if (host) {
    if (!ready) {
      // Right after the Host connected the missing pieces are normal boot steps:
      // progress, not a warning. After the grace window, or for a definite
      // failure, the clients' own attention texts apply.
      if (preparation?.transient && Number.isFinite(hostConnectedAt) && now - hostConnectedAt >= 0 && now - hostConnectedAt < preparingMs) return { state: 'preparing', reason: preparation.transient };
      return { state: 'host_wait', reason: 'host_preparing' };
    }
    return session ? { state: 'in_use', reason: 'session_active' } : { state: 'ready', reason: failed ? 'power_task_failed' : 'host_ready' };
  }
  const rebooted = recent(bootMs) && lastOk.action === 'reboot';
  const started = recent(bootMs) && lastOk.action === 'start';
  const booting = rebooted || started || (Number.isFinite(uptimeSeconds) && uptimeSeconds * 1000 < bootMs);
  if (booting) {
    if (agent === 'up') return { state: 'host_wait', reason: 'agent_up' };
    return { state: 'windows_starting', reason: rebooted ? 'rebooting' : 'booting' };
  }
  if (droppedAt !== null && droppedAt !== undefined) {
    // The Host went away without the broker asking for it.
    if (agent === 'down') return { state: 'windows_starting', reason: 'restarting' };
    return { state: 'host_lost', reason: agent === 'up' ? 'host_disconnected_vm_healthy' : 'host_disconnected' };
  }
  return { state: 'host_offline', reason: agent === 'up' ? 'agent_up_no_host' : 'host_not_connected' };
}

export class VmStates {
  constructor({ store, power, relay, onChange = () => {}, now = Date.now, schedule = SCHEDULE, watchMs = 180000, bootMs = 300000, preparingMs = 180000, maxWatches = 64, agentRetryMs = 600000, sleep = ms => new Promise(resolve => setTimeout(resolve, ms)) }) {
    Object.assign(this, { store, power, relay, onChange, now, schedule, watchMs, bootMs, preparingMs, maxWatches, sleep });
    this.agentDeniedAt = 0;
    this.agentRetryMs = agentRetryMs;
    this.mem = new Map();
    this.watches = new Map();
    this.closed = false;
  }
  // The identity's refusal is remembered (no error spam) but retried after a
  // while, so an administrator who adds the privilege is picked up without a
  // broker restart; saving the Proxmox connection also clears it.
  agentRefused() { return this.agentDeniedAt > 0 && this.now() - this.agentDeniedAt < this.agentRetryMs; }
  resetAgentDenial() { this.agentDeniedAt = 0; }
  memory(id) {
    let value = this.mem.get(id);
    if (!value) { value = { revision: 0, key: null, agent: null, agentEnabled: null, droppedAt: null }; this.mem.set(id, value); }
    return value;
  }
  // --- signals --------------------------------------------------------------
  hostEvent(id, kind, detail) {
    const memory = this.memory(id);
    if (kind === 'connected' || kind === 'ready') { memory.droppedAt = null; memory.agent = null; }
    if (kind === 'connected') { memory.reconnected = true; memory.hostConnectedAt = this.now(); }
    if (kind === 'disconnected') memory.hostConnectedAt = null;
    if (kind === 'disconnected' && detail?.unexpected && !this.power.active(id)) {
      memory.droppedAt = this.now();
      this.onChange(id);
      return this.watch(id);
    }
    this.onChange(id);
    return undefined;
  }
  // A power request the broker accepted: follow it until the VM really is in
  // the state it was asked for.
  powerEvent(id) {
    const memory = this.memory(id);
    memory.droppedAt = null; memory.agent = null; memory.reconnected = false; memory.hostConnectedAt = null;
    this.onChange(id);
    return this.watch(id);
  }
  watch(id) {
    if (this.closed) return;
    const existing = this.watches.get(id);
    if (existing) { existing.until = this.now() + this.watchMs; return existing.done; }
    if (this.watches.size >= this.maxWatches) return undefined;
    const watch = { until: this.now() + this.watchMs, done: null };
    this.watches.set(id, watch);
    watch.done = this.run(id, watch).catch(() => {}).finally(() => { if (this.watches.get(id) === watch) this.watches.delete(id); });
    return watch.done;
  }
  async run(id, watch) {
    const memory = this.memory(id);
    let step = 0;
    while (!this.closed && this.now() < watch.until) {
      await this.sleep(this.schedule[Math.min(step++, this.schedule.length - 1)]);
      if (this.closed) return;
      let changed = false;
      const active = this.power.active(id);
      if (active) {
        const before = active.status;
        const polled = await this.power.poll(active).catch(() => null);
        if (polled && (polled.status !== before || !polled.active)) changed = true;
      }
      let probe = null;
      try { probe = await this.power.probe(id); } catch { /* The next step asks again; the slow sync is the backstop. */ }
      if (probe?.changed) changed = true;
      if (probe) memory.probe = { uptimeSeconds: probe.uptimeSeconds, at: this.now() };
      const state = probe?.state ?? this.power.view(id, null).power.state;
      const host = this.relay.hostConnected(id);
      if (state === 'running' && !host && probe) changed = await this.checkAgent(id, memory, probe) || changed;
      if (state !== 'running') memory.agent = null;
      if (changed) this.onChange(id);
      if (this.finished(id, state, host)) { this.onChange(id); return; }
    }
    this.onChange(id);
  }
  // The guest agent is only asked while Windows' state is in question, only
  // for VMs whose Proxmox config enables it, and never again after the
  // identity was refused (no error spam; see docs).
  async checkAgent(id, memory, probe) {
    if (this.agentRefused() || typeof this.power.proxmox.agentPing !== 'function') return false;
    const config = this.power.config();
    try {
      if (memory.agentEnabled === null) memory.agentEnabled = await this.power.proxmox.agentEnabled(config.value, probe.node, probe.vmid);
      if (!memory.agentEnabled) return false;
      const result = await this.power.proxmox.agentPing(config.value, probe.node, probe.vmid);
      if (result === 'denied') { this.agentDeniedAt = this.now(); memory.agent = null; return false; }
      this.agentDeniedAt = 0;
      const before = memory.agent;
      memory.agent = result;
      return before !== result;
    } catch { return false; }
  }
  finished(id, state, host) {
    if (this.power.active(id)) return false;
    const { last } = this.power.taskState(id);
    const settling = last && last.status === 'stopped' && last.exit_status === 'OK' && this.now() - last.checked_at < this.power.settleMs;
    if (state === 'stopped') return !host && !(settling && last.action === 'start');
    // A reboot just acknowledged leaves the old Host connected until Windows drops it.
    if (state === 'running' && host && this.relay.hostReady(id) && !(settling && (last.action === 'shutdown' || last.action === 'stop' || last.action === 'reboot'))) return true;
    return false;
  }
  // The newer of the inventory's uptime and the last per-VM probe's, advanced
  // by the time since it was read.
  uptime(memory, desktop) {
    const sample = desktop.resources, sampleAt = sample?.checkedAt ? Date.parse(sample.checkedAt) : 0;
    const probe = memory.probe;
    if (probe && Number.isFinite(probe.uptimeSeconds) && probe.at >= sampleAt) return probe.uptimeSeconds + Math.max(0, (this.now() - probe.at) / 1000);
    return sample?.uptimeSeconds;
  }
  // --- output ---------------------------------------------------------------
  decorate(desktops) {
    return desktops.map(desktop => {
      const id = desktop.id, memory = this.memory(id);
      if (desktop.kind !== 'vm') return desktop;
      const verdict = deriveVmState({
        now: this.now(), power: desktop.powerState, powerStale: desktop.powerStale, powerError: desktop.powerError,
        task: this.power.taskState(id), uptimeSeconds: this.uptime(memory, desktop), host: desktop.host?.connected === true,
        ready: desktop.canConnect === true, session: this.relay.hasSession(id), agent: memory.agent, droppedAt: memory.droppedAt, reconnected: memory.reconnected === true, bootMs: this.bootMs, settleMs: this.power.settleMs, hostConnectedAt: memory.hostConnectedAt ?? null, preparation: classifyHostPreparation(desktop), preparingMs: this.preparingMs,
      });
      const key = `${verdict.state}:${verdict.reason}`;
      if (memory.key !== key) { memory.key = key; memory.revision = Math.max(memory.revision + 1, this.now()); }
      if (desktop.powerState === 'stopped') { memory.droppedAt = null; memory.agent = null; memory.probe = null; }
      return { ...desktop, vmState: verdict.state, vmStateReason: verdict.reason, vmStateRevision: memory.revision };
    });
  }
  close() { this.closed = true; this.watches.clear(); }
}
