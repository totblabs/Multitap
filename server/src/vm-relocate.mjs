// A registered desktop is bound to the node and VM ID it was approved on
// (host-requests.mjs) and every Proxmox read uses that exact place. A
// migration or HA recovery moves the VM to another node, and a backup restored
// to a new VM ID gives it another number; the desktop then showed unknown and
// power and Console failed. This finds such a VM again by the SMBIOS UUID the
// desktop recorded at approval (the server finds a
// VM by its unique ID) and moves the desktop row to it, keeping the row,
// its assignments, permissions and Host registration (store.mjs
// relocateRegisteredVm, audited as desktop_vm_relocated).
//
// Bounded like the request lookup (host-vm-lookup.mjs):
// - only after an exact read failed as if the VM was not there (the codes
//   below), never for a UUID mismatch (the VM ID now belongs to another VM:
//   that desktop stays quarantined) or for an unreachable Proxmox;
// - only for a desktop with a recorded Proxmox UUID (pve_machine_uuid);
// - at most once per desktop per minIntervalMs, and after a lookup that did
//   not find it, ambiguous or not, again no sooner than retryMs (an explicit
//   administrator refresh passes `force` and waits the minute only);
// - one lookup at a time, each limited to scanMs and maxVms config reads;
// - callers wait at most their own deadline (waitMs or their signal); a
//   lookup they stopped waiting for still finishes and updates the row.
// Proxmox answers a config read of a VM that is not on the asked node with an
// HTTP error (proxmox_request_failed); a template there reads as
// proxmox_vm_not_found (proxmox.mjs _registeredTarget).
export const relocatableErrors = new Set(['proxmox_request_failed', 'proxmox_vm_not_found']);

const sleep = ms => new Promise(resolve => { const timer = setTimeout(resolve, ms); timer.unref?.(); });

export class VmRelocation {
  constructor({ store, proxmox, now = Date.now, minIntervalMs = 60000, retryMs = 600000, scanMs = 15000, maxVms = 1000, actor = 'server' } = {}) {
    Object.assign(this, { store, proxmox, now, minIntervalMs, retryMs, scanMs, maxVms, actor });
    this.attempts = new Map(); // desktopId -> { at, retryAfter }
    this.running = null;
  }
  due(desktopId, now, force) {
    const last = this.attempts.get(desktopId);
    return !last || now - last.at >= (force ? this.minIntervalMs : Math.max(this.minIntervalMs, last.retryAfter));
  }
  // failed: registered targets (store.registeredVmTargets rows) with the
  // `error` code of their failed exact read. Returns
  // [{ desktopId, state, from, to? }] for the desktops looked up by this call
  // (or by the lookup it joined); state is moved, present, replaced,
  // ambiguous, not_found, conflict, changed or unavailable.
  async relocate(config, failed, { revision = this.store.setting('proxmox'), signal, waitMs = Infinity, force = false } = {}) {
    if (typeof this.proxmox?.locateMachines !== 'function' || !Array.isArray(failed) || !revision) return [];
    const ids = new Set(failed.map(target => target?.desktopId));
    if (this.running) return this.wait(this.running.promise, { signal, waitMs }).then(results => (results ?? []).filter(result => ids.has(result.desktopId)));
    const now = this.now();
    for (const [id, last] of this.attempts) if (now - last.at >= Math.max(this.minIntervalMs, last.retryAfter)) this.attempts.delete(id);
    const seen = new Set();
    const wanted = failed.filter(target => {
      if (!target || !relocatableErrors.has(target.error) || typeof target.desktopId !== 'string' || seen.has(target.vmid) || !this.due(target.desktopId, now, force)) return false;
      const recorded = this.store.db.prepare("SELECT pve_machine_uuid FROM desktops WHERE id=? AND kind='vm' AND pve_node=? AND pve_vmid=?").get(target.desktopId, target.node, target.vmid)?.pve_machine_uuid;
      if (typeof recorded !== 'string' || recorded.toLowerCase() !== target.expectedUuid) return false;
      seen.add(target.vmid); return true;
    });
    if (!wanted.length) return [];
    for (const target of wanted) this.attempts.set(target.desktopId, { at: now, retryAfter: 0 });
    const promise = this.run(config, revision, wanted).finally(() => { if (this.running?.promise === promise) this.running = null; });
    this.running = { promise };
    promise.catch(() => {});
    return this.wait(promise, { signal, waitMs });
  }
  async wait(promise, { signal, waitMs }) {
    const stops = [promise];
    if (Number.isFinite(waitMs)) stops.push(sleep(Math.max(0, waitMs)).then(() => null));
    if (signal) stops.push(new Promise(resolve => { if (signal.aborted) resolve(null); else signal.addEventListener('abort', () => resolve(null), { once: true }); }));
    try { return (await Promise.race(stops)) ?? []; } catch { return []; }
  }
  async run(config, revision, wanted) {
    const from = target => ({ node: target.node, vmid: target.vmid });
    let found;
    try {
      found = await this.proxmox.locateMachines(config, wanted.map(({ node, vmid, expectedUuid }) => ({ node, vmid, expectedUuid })), { signal: AbortSignal.timeout(this.scanMs), maxVms: this.maxVms });
      if (!Array.isArray(found) || found.length !== wanted.length) throw new Error('invalid_relocation');
    } catch { found = wanted.map(() => ({ state: 'unavailable' })); }
    const settingsChanged = this.store.setting('proxmox') !== revision;
    return wanted.map((target, index) => {
      const result = found[index];
      let state = ['moved', 'present', 'replaced', 'ambiguous', 'not_found'].includes(result?.state) ? result.state : 'unavailable';
      if (state === 'moved') {
        if (settingsChanged) state = 'unavailable';
        else {
          try { state = this.store.relocateRegisteredVm(target, { node: result.node, vmid: result.vmid }, this.actor, this.now()); }
          catch { state = 'unavailable'; }
        }
      }
      // A full lookup that did not place the VM is not repeated every minute.
      if (['ambiguous', 'not_found', 'conflict'].includes(state)) this.attempts.set(target.desktopId, { at: this.now(), retryAfter: this.retryMs });
      return { desktopId: target.desktopId, state, from: from(target), ...(state === 'moved' ? { to: { node: result.node, vmid: result.vmid } } : {}) };
    });
  }
  // One desktop whose exact read at `from` failed with `error`: its new place
  // when it was moved (by this lookup or one that finished meanwhile), else
  // null. Callers read the VM again there and re-check its identity.
  async relocateDesktop(config, desktopId, from, error, options = {}) {
    const target = this.store.registeredVmTargets().find(value => value.desktopId === desktopId);
    if (!target || !relocatableErrors.has(error)) return null;
    if (target.node !== from?.node || target.vmid !== from?.vmid) return { node: target.node, vmid: target.vmid };
    const result = (await this.relocate(config, [{ ...target, error }], options)).find(value => value.desktopId === desktopId);
    return result?.state === 'moved' ? result.to : null;
  }
}

// The registered inventory read with relocation: reads every target, looks up
// the ones that failed as missing, and reads the moved ones again at their new
// place. Returns the registered targets the result belongs to (they changed
// when a desktop moved), the observations in that order, and the relocations.
export async function readRegisteredInventory({ proxmox, relocation, store, config, revision, targets, enrich, waitMs = Infinity, force = false }) {
  const read = list => list.length ? proxmox.inventory(config, { ...(enrich === undefined ? {} : { enrich }), targets: list.map(({ node, vmid, expectedUuid }) => ({ node, vmid, expectedUuid })) }) : Promise.resolve([]);
  const vms = await read(targets);
  if (!relocation || !Array.isArray(vms) || vms.length !== targets.length) return { targets, vms, relocations: [] };
  const failed = targets.map((target, index) => ({ ...target, error: vms[index]?.node === target.node && vms[index]?.vmid === target.vmid ? vms[index].powerError : undefined }))
    .filter(target => relocatableErrors.has(target.error));
  if (!failed.length) return { targets, vms, relocations: [] };
  const relocations = await relocation.relocate(config, failed, { revision, waitMs, force });
  if (!relocations.some(result => result.state === 'moved')) return { targets, vms, relocations };
  const key = target => JSON.stringify(target), previous = new Map(targets.map((target, index) => [key(target), vms[index]]));
  const next = store.registeredVmTargets(), reread = next.filter(target => !previous.has(key(target)));
  const fresh = await read(reread);
  if (!Array.isArray(fresh) || fresh.length !== reread.length) return { targets: next, vms: fresh, relocations };
  const fetched = new Map(reread.map((target, index) => [key(target), fresh[index]]));
  return { targets: next, vms: next.map(target => previous.get(key(target)) ?? fetched.get(key(target))), relocations };
}
