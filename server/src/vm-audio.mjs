import { HttpError } from './errors.mjs';

export const vmAudioProtocol = 'vm-audio-v1';

// The VM sound card, observed only. Multitap does not add one: the
// administrator adds an audio device to the VM in Proxmox (Hardware → Add →
// Audio Device), and the integration role does not ask for
// VM.Config.HWType.
//
// What stays: each VM's observed state (present, pending, absent) for the
// badge of the client list, read with VM.Audit alone.
//
// What older clients still call (they have a sound-card dialog): the list
// says soundCardAddAllowed false and canAdd false, so they do not offer the
// dialog; a status or add request that comes
// anyway gets the refusal those clients already name, 403
// vm_audio_permission_denied, after the same account and assignment checks as
// before. The stored per-assignment sound_card_add flag is kept and not read.
export class VmAudioControl {
  constructor({ store, vault, proxmox, relay, now = Date.now, onChange = () => {}, stateTtlMs = 600000, dueMs = 120000 }) {
    Object.assign(this, { store, vault, proxmox, relay, now, onChange, stateTtlMs, dueMs });
    // No VM configuration is changed any more. Both stay empty, in the shape
    // the relay's busy checks and the activity snapshot read.
    this.operations = new Map(); this.uncertain = new Set();
    // Each VM's sound-card state for the client list: desktop id → { state,
    // locked, digest, checkedAt }. `due` holds VMs to re-read soon (a power
    // action) until its time passes.
    this.states = new Map(); this.due = new Map(); this.refreshing = null;
  }
  // The list entry (vm-audio-v1 shape) for an account the VM is assigned to,
  // or undefined when the state is unknown. Nothing can be added from a client.
  listView(desktopId, viewer) {
    const value = this.states.get(desktopId);
    if (!value || !viewer) return undefined;
    return { state: value.state, canAdd: false };
  }
  record(desktopId, value) {
    const previous = this.states.get(desktopId);
    this.states.set(desktopId, { ...value, checkedAt: this.now() });
    if (previous?.state !== value.state || previous?.locked !== value.locked) this.onChange();
  }
  // From one VM's effective config (pending values applied): audio0 there is
  // present or pending, and only then is the current config read to tell which.
  // The config digest changes with any edit, so an unchanged digest reuses it.
  async classify(config, desktopId, node, vmid, effective) {
    const locked = Object.hasOwn(effective, 'lock'), digest = typeof effective.digest === 'string' ? effective.digest : null;
    let state = 'absent';
    if (Object.hasOwn(effective, 'audio0')) {
      const previous = this.states.get(desktopId);
      state = digest && previous?.digest === digest && previous.state !== 'absent' ? previous.state
        : Object.hasOwn(await this.proxmox.vmConfig(config, node, vmid, { current: true }), 'audio0') ? 'present' : 'pending';
    }
    this.record(desktopId, { state, locked, digest });
  }
  // The admin inventory sync already read each VM's effective config.
  async observeInventory(config, vms) {
    for (const vm of vms) {
      if (!vm.audioConfig) continue;
      const row = this.store.db.prepare("SELECT id FROM desktops WHERE kind='vm' AND pve_vmid=?").get(vm.vmid);
      if (row) try { await this.classify(config, row.id, vm.node, vm.vmid, vm.audioConfig); } catch { /* the next refresh retries */ }
    }
  }
  markDue(desktopId) { this.due.set(desktopId, this.now() + this.dueMs); }
  // Background refresh of stale or due VMs, one at a time per broker; at most
  // `limit` config reads per run. Never awaited by a client request.
  refreshStale({ limit = 8 } = {}) {
    if (this.refreshing) return this.refreshing;
    let config;
    try { config = this.config(); } catch { return Promise.resolve(); }
    const now = this.now();
    for (const [id, until] of this.due) if (until < now) this.due.delete(id);
    const rows = this.store.db.prepare("SELECT id,pve_node,pve_vmid FROM desktops WHERE kind='vm' AND pve_vmid IS NOT NULL AND pve_available=1 ORDER BY pve_vmid").all()
      .filter(row => { const value = this.states.get(row.id); return !value || now - value.checkedAt >= this.stateTtlMs || (this.due.has(row.id) && now - value.checkedAt >= 5000); }).slice(0, limit);
    this.refreshing = (async () => {
      for (const row of rows) {
        try { await this.classify(config.value, row.id, row.pve_node, row.pve_vmid, await this.proxmox.vmConfig(config.value, row.pve_node, row.pve_vmid)); }
        catch { /* unknown stays unknown; the next run retries */ }
      }
    })().finally(() => { this.refreshing = null; });
    return this.refreshing;
  }
  authorized(authorize) {
    const current = authorize(), desktop = current.desktop;
    if (desktop.kind !== 'vm' || !Number.isSafeInteger(desktop.pve_vmid) || desktop.pve_vmid < 100) throw new HttpError(409, 'vm_audio_vm_only');
    return current;
  }
  config() {
    const revision = this.store.setting('proxmox');
    if (!revision || !this.vault) throw new HttpError(409, 'proxmox_not_configured');
    return { revision, value: this.vault.open(revision) };
  }
  // GET and POST /api/v1/desktops/<id>/audio of released clients. Whoever may
  // not see the desktop still gets 401/404 and a local PC 409 vm_audio_vm_only,
  // as before; everyone else is told that this account cannot add a sound card.
  // Nothing is read from or sent to Proxmox.
  refuse(authorize) {
    this.authorized(authorize);
    throw new HttpError(403, 'vm_audio_permission_denied');
  }
  async status(authorize) { return this.refuse(authorize); }
  async add(authorize) { return this.refuse(authorize); }
}
