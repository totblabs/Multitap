import { createHash, randomUUID } from 'node:crypto';
import { HttpError } from './errors.mjs';
import { mergeResources, pcResources } from './host-resources.mjs';
import { readRegisteredInventory } from './vm-relocate.mjs';

// What an assigned account may ask for its VM, as in Proxmox: 시작, 종료
// (graceful shutdown), 재부팅 and 정지 (stop: immediate power-off; an
// assigned account may ask for it too, not only an administrator).
// Reset, suspend and any
// other Proxmox action stay refused for everyone.
const actions = new Set(['start', 'shutdown', 'reboot', 'stop']);
const taskTypes = { start:['qmstart','hastart'], shutdown:['qmshutdown','hastop'], reboot:['qmreboot'], stop:['qmstop','hastop'] };
const validTask = (node, task, vmid, action) => {
  if (typeof node !== 'string' || !/^[A-Za-z0-9_.-]+$/.test(node) || typeof task !== 'string' || !/^UPID:[A-Za-z0-9_.:@!-]+:$/.test(task) || task.length > 400) return false;
  const parts = task.split(':');
  return parts.length === 9 && parts[1] === node && parts[6] === String(vmid) && !!taskTypes[action]?.includes(parts[5]);
};
const stamp = value => createHash('sha256').update(value).digest('hex');
const iso = value => value == null ? null : new Date(value).toISOString();
const safeError = error => error instanceof HttpError && /^proxmox_[a-z_]+$/.test(error.code) ? error.code : 'proxmox_unreachable';
// The state a finished, successful action leaves the VM in (a reboot leaves it running).
const resultState = { start: 'running', shutdown: 'stopped', stop: 'stopped' };
const maxUptimeSeconds = 100 * 366 * 86400;
export class PowerControl {
  // settleMs: Proxmox's cluster resource list (pvestatd, ~10 s cycle) shows a
  // VM's new state only some seconds after its start/shutdown task finished.
  // relocation (vm-relocate.mjs): a registered VM that is not where it was
  // registered is looked up by its UUID. The refresh waits relocationWaitMs
  // for that (a move to another node is one listing and one config read); a
  // longer lookup still updates the row and the next refresh reads it.
  constructor({ store, vault, proxmox, cacheMs = 2000, settleMs = 20000, now = Date.now, mutationGuard = () => {}, relocation = null, relocationWaitMs = 3000 }) {
    this.store = store; this.vault = vault; this.proxmox = proxmox; this.cacheMs = cacheMs; this.settleMs = settleMs; this.now = now; this.mutationGuard = mutationGuard;
    this.relocation = relocation; this.relocationWaitMs = relocationWaitMs;
    this.attemptAt = null; this.snapshotRevision = null; this.snapshotTargets = null; this.snapshotError = null; this.pendingSnapshot = null;
    this.polls = new Map(); this.mutations = 0; this.probed = new Map();
    this.resources = new Map(); this.resourcesRevision = null;
    this.hostReport = () => null; // desktop id -> fresh Host-measured resources (http.mjs wires the relay)
  }
  config() {
    const revision = this.store.setting('proxmox');
    if (!revision || !this.vault) throw new HttpError(409, 'proxmox_not_configured');
    return { revision, value: this.vault.open(revision) };
  }
  current(revision) { if (this.store.setting('proxmox') !== revision) throw new HttpError(409, 'proxmox_settings_changed'); }
  // Registration owns the list. Poll only those exact node/VM/UUID bindings;
  // a different registration invalidates the shared observation cache.
  async refresh({ maxAgeMs = this.cacheMs } = {}) {
    const revision = this.store.setting('proxmox');
    let targets = this.store.registeredVmTargets(), targetStamp = JSON.stringify(targets);
    if (this.pendingSnapshot) { await this.pendingSnapshot; return this.refresh({ maxAgeMs }); }
    if (this.snapshotRevision === revision && this.snapshotTargets === targetStamp && this.attemptAt !== null && this.now() - this.attemptAt < maxAgeMs) return;
    this.pendingSnapshot = (async () => {
      this.attemptAt = this.now(); this.snapshotRevision = revision; this.snapshotTargets = targetStamp;
      try {
        const config = this.config();
        const read = await readRegisteredInventory({ proxmox: this.proxmox, relocation: this.relocation, store: this.store, config: config.value, revision: config.revision, targets, enrich: false, waitMs: this.relocationWaitMs });
        const vms = read.vms;
        // A desktop followed its VM to a new place: these are its targets now.
        if (read.targets !== targets) { targets = read.targets; targetStamp = JSON.stringify(targets); this.snapshotTargets = targetStamp; }
        this.current(config.revision);
        const seen = new Set();
        const expected = new Map(targets.map(target => [target.vmid, target]));
        if (!Array.isArray(vms) || vms.length !== targets.length || vms.some(vm => !vm || !Number.isSafeInteger(vm.vmid) || !expected.has(vm.vmid) || vm.node !== expected.get(vm.vmid).node || !['running','stopped','unknown'].includes(vm.status) || seen.has(vm.vmid) || !seen.add(vm.vmid) || (vm.powerError !== undefined && (typeof vm.powerError !== 'string' || !/^proxmox_[a-z_]+$/.test(vm.powerError))))) throw new HttpError(502, 'proxmox_invalid_response');
        const now = this.now();
        this.store.transaction(() => {
          this.current(config.revision);
          if (JSON.stringify(this.store.registeredVmTargets()) !== targetStamp) throw new HttpError(409, 'proxmox_targets_changed');
          const update = this.store.db.prepare("UPDATE desktops SET pve_power_status=?,pve_power_checked_at=?,pve_power_error=? WHERE id=? AND kind='vm' AND pve_vmid=? AND pve_node=?");
          for (const vm of vms) {
            const target = expected.get(vm.vmid);
            if (this.store.observeRegisteredVmAvailability(target, vm) === false) throw new HttpError(409, 'proxmox_targets_changed');
            this.probed.delete(vm.vmid);
            update.run(vm.status, now, vm.powerError ?? null, target.desktopId, vm.vmid, vm.node);
            // The same config read says whether the QEMU guest agent option is on.
            if (!vm.powerError && typeof vm.qemuAgent === 'boolean') this.store.qemuAgentOption?.set(target.desktopId, vm.qemuAgent);
            else this.store.qemuAgentOption?.delete(target.desktopId);
          }
          if (vms.some(vm => Object.hasOwn(vm, 'tags') || Object.hasOwn(vm, 'pool'))) this.store.applyOrganization(vms.filter(vm => !vm.powerError));
        });
        // Keep an earlier failed target's sample as stale, never as a new
        // measurement. Different installation settings never share samples.
        const whole = (value, max = Number.MAX_SAFE_INTEGER) => Number.isSafeInteger(value) && value > 0 && value <= max ? value : null;
        const running = vm => vm.status === 'running';
        const resources = this.resourcesRevision === config.revision ? new Map([...this.resources].filter(([vmid]) => expected.has(vmid))) : new Map();
        for (const vm of vms) {
          if (vm.powerError) continue;
          resources.set(vm.vmid, {
          source: 'proxmox', checkedAt: iso(now),
          cpuUsagePercent: running(vm) && typeof vm.cpuUsage === 'number' && Number.isFinite(vm.cpuUsage) && vm.cpuUsage >= 0 && vm.cpuUsage <= 1 ? vm.cpuUsage * 100 : null,
          vcpus: whole(vm.vcpus, 4096),
          memoryUsedBytes: running(vm) && Number.isSafeInteger(vm.memoryUsedBytes) && vm.memoryUsedBytes >= 0 ? vm.memoryUsedBytes : null,
          memoryTotalBytes: whole(vm.memoryBytes),
          // Seconds since the VM was started (Proxmox `uptime`), only while
          // it runs; anything outside 0..100 years is not a measurement.
          uptimeSeconds: running(vm) && Number.isSafeInteger(vm.uptimeSeconds) && vm.uptimeSeconds >= 0 && vm.uptimeSeconds <= maxUptimeSeconds ? vm.uptimeSeconds : null,
          });
        }
        this.resources = resources;
        this.resourcesRevision = config.revision;
        this.snapshotError = null;
      } catch (error) { this.snapshotError = safeError(error); }
    })();
    try { await this.pendingSnapshot; } finally { this.pendingSnapshot = null; }
  }
  taskPublic(row) {
    return row ? { id: row.id, desktopId: row.desktop_id, action: row.action, status: row.status,
      exitStatus: row.exit_status, submittedAt: iso(row.submitted_at), checkedAt: iso(row.checked_at) } : null;
  }
  active(id) { return this.store.db.prepare('SELECT * FROM power_tasks WHERE desktop_id=? AND active=1').get(id); }
  // A successful start/shutdown (any account) whose result the power snapshot
  // does not show yet: the VM's state is still changing. Deciding another
  // action on that stale snapshot submitted a second start to a running VM
  // ("VM <id> already running" -> power_task_failed in client diagnostics).
  settling(id, power) {
    const last = this.store.db.prepare('SELECT action,status,exit_status,checked_at FROM power_tasks WHERE desktop_id=? AND active=0 ORDER BY submitted_at DESC,rowid DESC LIMIT 1').get(id);
    const expected = last && resultState[last.action];
    return !!expected && last.status === 'stopped' && last.exit_status === 'OK' && last.checked_at !== null &&
      this.now() - last.checked_at < this.settleMs && power.state !== expected;
  }
  // Any account's tasks for the VM: the one in flight and the last finished.
  taskState(id) {
    const last = this.store.db.prepare("SELECT action,status,exit_status,checked_at FROM power_tasks WHERE desktop_id=? AND active=0 AND status<>'superseded' ORDER BY submitted_at DESC,rowid DESC LIMIT 1").get(id);
    return { active: this.active(id) ?? null, last: last ?? null };
  }
  // The live status of this one VM (a single Proxmox call, no inventory),
  // written to the same power record the inventory sync keeps. Null when the
  // adapter has no per-VM status or the VM is unknown.
  async probe(id) {
    if (typeof this.proxmox.vmStatus !== 'function') return null;
    const target = this.store.registeredVmTargets().find(value => value.desktopId === id);
    if (!target) return null;
    const config = this.config();
    const live = await this.proxmox.vmStatus(config.value, target.node, target.vmid);
    this.current(config.revision);
    if (JSON.stringify(this.store.registeredVmTargets().find(value => value.desktopId === id)) !== JSON.stringify(target)) throw new HttpError(409, 'proxmox_targets_changed');
    if (!['running', 'stopped'].includes(live.status)) return null;
    const before = this.store.db.prepare('SELECT pve_power_status FROM desktops WHERE id=?').get(id)?.pve_power_status;
    this.store.db.prepare("UPDATE desktops SET pve_power_status=?,pve_power_checked_at=?,pve_power_error=NULL WHERE id=? AND kind='vm'").run(live.status, this.now(), id);
    this.probed.set(target.vmid, { state: live.status, at: this.now() });
    return { state: live.status, paused: live.paused, uptimeSeconds: live.uptimeSeconds, changed: before !== live.status, node: target.node, vmid: target.vmid };
  }
  latest(id, actor) { return this.store.db.prepare('SELECT * FROM power_tasks WHERE desktop_id=? AND user_id=? ORDER BY submitted_at DESC,rowid DESC LIMIT 1').get(id, actor); }
  supersedable(row, desktop, actor) {
    const revision = this.store.setting('proxmox');
    return !!row && row.active === 1 && row.user_id === actor && row.action === 'shutdown' &&
      ['submitted','running'].includes(row.status) && !!revision && stamp(revision) === row.config_stamp &&
      validTask(row.pve_node, row.pve_task, desktop.pve_vmid, 'shutdown') && row.pve_task.split(':')[5] === 'qmshutdown';
  }
  view(id, actor, staleMs = this.cacheMs * 2) {
    const desktop = this.store.db.prepare('SELECT pve_vmid,pve_power_status,pve_power_checked_at,pve_power_error FROM desktops WHERE id=?').get(id);
    const stale = !!this.snapshotError || !desktop?.pve_power_checked_at || this.now() - desktop.pve_power_checked_at >= staleMs;
    const power = { state: desktop?.pve_power_status ?? 'unknown', checkedAt: iso(desktop?.pve_power_checked_at), stale,
      error: this.snapshotError ?? desktop?.pve_power_error ?? null };
    const pending = this.active(id);
    // Whether this account may stop (정지) the VM now: the conditions submit
    // checks before Proxmox — a running VM with a fresh snapshot, idle or
    // still in this account's own confirmed qmshutdown (a stuck 종료), and no
    // settling result. Clients offer 정지 only while it is true; a broker
    // before client stop never reported it to a client.
    const canForceStop = power.state === 'running' && !power.stale && !power.error && (!pending || this.supersedable(pending, desktop, actor)) && !this.settling(id, power);
    const sample = this.resourcesRevision === this.store.setting('proxmox') ? this.resources.get(desktop?.pve_vmid) : null;
    const resources = sample ? { ...mergeResources(sample, desktop?.pve_power_status === 'running' ? this.hostReport(id) : null), stale: stale || sample.checkedAt !== power.checkedAt || !!power.error } : null;
    return { power, powerTask: this.taskPublic(this.latest(id, actor)), powerBusy: !!pending || this.settling(id, power), canForceStop, resources };
  }
  // The last CPU/memory/uptime sample of one VM for the administrator's 홈
  // (admin-resources.mjs). No Proxmox call. Null when these Proxmox settings
  // have none, the last read failed, or the sample is older than maxAgeMs.
  latestSample(vmid, maxAgeMs) {
    if (this.snapshotError || this.resourcesRevision === null || this.resourcesRevision !== this.store.setting('proxmox')) return null;
    const sample = this.resources.get(vmid), at = sample ? Date.parse(sample.checkedAt) : NaN;
    return Number.isFinite(at) && this.now() - at <= maxAgeMs ? sample : null;
  }
  decorate(desktops, actor, { staleMs } = {}) {
    return desktops.map(desktop => {
      // An approved PC carries its own power view (pc-power.mjs), never VM
      // power fields; CPU/memory are its Host's own report.
      if (desktop.kind === 'pc') return { ...desktop, ...(this.pc ? { pcPower: this.pc.listView(desktop.id, actor) } : {}), resources: pcResources(this.hostReport(desktop.id)) };
      const { power, powerTask, powerBusy, canForceStop, resources } = this.view(desktop.id, actor, staleMs);
      return { ...desktop, powerState: power.state, powerCheckedAt: power.checkedAt, powerStale: power.stale, powerError: power.error, powerTask, powerBusy, powerCanForceStop: canForceStop, resources };
    });
  }
  async poll(row) {
    if (!row || !row.active) return row;
    if (this.polls.has(row.id)) return this.polls.get(row.id);
    if (this.polls.size >= 16) throw new HttpError(503, 'power_busy');
    if (row.checked_at !== null && this.now() - row.checked_at < 1000) return row;
    const promise = (async () => {
      if (!row.pve_task) {
        if (this.now() >= row.guard_until) this.store.db.prepare("UPDATE power_tasks SET status='unknown',active=0,checked_at=? WHERE id=? AND active=1").run(this.now(), row.id);
      } else {
        const config = this.config();
        if (stamp(config.revision) !== row.config_stamp) {
          this.store.db.prepare("UPDATE power_tasks SET status='unknown',active=?,checked_at=? WHERE id=? AND active=1").run(this.now() < row.guard_until ? 1 : 0, this.now(), row.id);
        } else {
          const result = await this.proxmox.task(config.value, row.pve_node, row.pve_task);
          this.current(config.revision);
          if (!result || !['running','stopped'].includes(result.status) || (result.status === 'stopped' && typeof result.exitStatus !== 'string')) throw new HttpError(502, 'proxmox_invalid_response');
          // A late poll must not reactivate a shutdown after an atomic stop
          // reservation moved its guard to the replacement task.
          this.store.db.prepare('UPDATE power_tasks SET status=?,active=?,exit_status=?,checked_at=? WHERE id=? AND active=1').run(result.status, result.status === 'stopped' ? 0 : 1, result.status === 'stopped' ? result.exitStatus.slice(0,200) : null, this.now(), row.id);
          if (result.status === 'stopped') this.attemptAt = null;
        }
      }
      return this.store.db.prepare('SELECT * FROM power_tasks WHERE id=?').get(row.id);
    })();
    this.polls.set(row.id, promise);
    try { return await promise; } finally { this.polls.delete(row.id); }
  }
  async status(authorize, actor, taskId) {
    const desktop = authorize();
    if (taskId !== undefined) {
      if (!/^[0-9a-f-]{36}$/.test(taskId)) throw new HttpError(404, 'power_task_not_found');
      const task = this.store.db.prepare('SELECT * FROM power_tasks WHERE id=? AND desktop_id=? AND user_id=?').get(taskId, desktop.id, actor);
      if (!task) throw new HttpError(404, 'power_task_not_found');
      const result = await this.poll(task); authorize(); await this.refresh(); authorize();
      return { ...this.view(desktop.id, actor), powerTask: this.taskPublic(result) };
    }
    await this.poll(this.active(desktop.id)); authorize(); await this.refresh(); authorize();
    return this.view(desktop.id, actor);
  }
  async submit(authorize, actor, action, { legacyAdmin = false } = {}) {
    // legacyAdmin is supplied only by the authenticated administration route,
    // never by a request body, and only shapes its response. Every account
    // gets the same actions on the VMs it is authorized for (assignment for
    // the client route); historical task records are retained unchanged.
    if (!actions.has(action)) throw new HttpError(400, 'invalid_power_action');
    let desktop = authorize();
    this.mutationGuard(desktop.id);
    if (this.mutations >= 4) throw new HttpError(503, 'power_busy');
    this.mutations++;
    try {
      await this.poll(this.active(desktop.id)); desktop = authorize();
      const pending = this.active(desktop.id);
      if (pending && !(action === 'stop' && this.supersedable(pending, desktop, actor))) throw new HttpError(409, 'power_in_progress');
      await this.refresh(); desktop = authorize();
      const before = this.view(desktop.id, actor).power;
      if (before.stale || before.error || before.state === 'unknown') throw new HttpError(503, 'power_status_unavailable');
      if (this.settling(desktop.id, before)) throw new HttpError(409, 'power_in_progress');
      if (before.state !== (action === 'start' ? 'stopped' : 'running')) throw new HttpError(409, 'invalid_power_state');
      const config = this.config(), now = this.now(), id = randomUUID();
      let superseded = null;
      this.mutationGuard(desktop.id);
      this.store.transaction(() => {
        const blocking = this.active(desktop.id);
        if (blocking && !(action === 'stop' && this.supersedable(blocking, desktop, actor))) throw new HttpError(409, 'power_in_progress');
        this.store.db.prepare('DELETE FROM power_tasks WHERE active=0 AND submitted_at<?').run(now - 7 * 86400000);
        this.store.db.prepare('DELETE FROM power_tasks WHERE id IN (SELECT id FROM power_tasks WHERE active=0 ORDER BY submitted_at DESC LIMIT -1 OFFSET 10000)').run();
        if (blocking) {
          superseded = blocking;
          this.store.db.prepare('UPDATE power_tasks SET active=0 WHERE id=? AND active=1').run(blocking.id);
        }
        if (this.store.db.prepare('SELECT count(*) AS n FROM power_tasks WHERE active=1').get().n >= 1000) throw new HttpError(503, 'power_busy');
        this.store.db.prepare("INSERT INTO power_tasks(id,desktop_id,user_id,action,status,submitted_at,guard_until,config_stamp) VALUES(?,?,?,?,'submitted',?,?,?)").run(id, desktop.id, actor, action, now, now + 30000, stamp(config.revision));
      });
      let result;
      try {
        result = await this.proxmox.power(config.value, desktop.pve_vmid, action, { node: desktop.pve_node, expectedUuid: desktop.pve_machine_uuid, overruleShutdown: !!superseded, beforeSubmit: () => { const current = authorize(); if (current.pve_node !== desktop.pve_node || current.pve_vmid !== desktop.pve_vmid || current.pve_machine_uuid !== desktop.pve_machine_uuid) throw new HttpError(409, 'desktop_changed'); this.current(config.revision); this.mutationGuard(desktop.id); } });
        if (!result || !validTask(result.node, result.task, desktop.pve_vmid, action)) throw new HttpError(502, 'proxmox_invalid_response');
      } catch (error) {
        const uncertain = error.code === 'proxmox_unreachable' || error.code === 'proxmox_invalid_response';
        this.store.transaction(() => {
          this.store.db.prepare("UPDATE power_tasks SET status=?,active=?,exit_status=?,checked_at=? WHERE id=?").run(uncertain ? 'unknown' : 'stopped', uncertain ? 1 : 0, uncertain ? null : 'request_rejected', this.now(), id);
          if (superseded) {
            if (uncertain) this.store.db.prepare("UPDATE power_tasks SET status='unknown',exit_status=NULL,checked_at=? WHERE id=? AND active=0").run(this.now(), superseded.id);
            else this.store.db.prepare('UPDATE power_tasks SET active=1 WHERE id=? AND active=0').run(superseded.id);
          }
        });
        this.store.audit('power_failed', actor, `${desktop.id}:${action}`);
        throw error;
      }
      this.store.transaction(() => {
        this.store.db.prepare('UPDATE power_tasks SET pve_node=?,pve_task=?,checked_at=? WHERE id=?').run(result.node, result.task, this.now(), id);
        if (superseded) {
          this.store.db.prepare("UPDATE power_tasks SET status='superseded',exit_status=NULL,checked_at=? WHERE id=? AND active=0").run(this.now(), superseded.id);
          this.store.audit('power_superseded', actor, `${desktop.id}:${superseded.id}:${id}`);
        }
        this.store.audit('power_submitted', actor, `${desktop.id}:${action}`);
      });
      authorize();
      const response = this.view(desktop.id, actor);
      return legacyAdmin ? { task: result.task, node: result.node, vmid: desktop.pve_vmid, action, status: 'submitted', ...response } : response;
    } finally { this.mutations--; }
  }
}
