import { HttpError } from './errors.mjs';

// Which Proxmox VM does a pending Host request come from? A request carries
// the machine's SMBIOS UUID, and Proxmox knows every VM's UUID, so the
// administrator does not have to type node and VM ID for a VM that never had
// a Host.
//
// Requests are unauthenticated claims, so their number must not decide how
// much Proxmox is asked. The unit of work is therefore one snapshot of the
// whole cluster (Proxmox.machineIndex: one resource listing, then one config
// read per VM, four at a time, capped by maxVms and scanMs) that answers every
// request at once:
// - at most one read runs at a time and the next one starts no sooner than
//   minIntervalMs after the last, whatever the number of requests or UUIDs;
// - a read happens only while an administrator is looking at a request that
//   the desktops table cannot place (refresh is called from the console's
//   request list, never from the Host-facing routes);
// - a UUID that a complete snapshot taken after its request did not contain
//   is not looked up again until the snapshot ages out (maxAgeMs).
// A snapshot only suggests. Approval still reads the one VM again
// (machineIdentity with expectedUuid) and keeps every other guard.
export class HostVmLookup {
  constructor({ proxmox, now = Date.now, minIntervalMs = 15000, maxAgeMs = 60000, keepMs = 600000, scanMs = 20000, maxVms = 1000 } = {}) {
    Object.assign(this, { proxmox, now, minIntervalMs, maxAgeMs, keepMs, scanMs, maxVms });
    this.snapshot = null; this.running = null; this.lastStartedAt = null;
  }
  // The snapshot of these exact Proxmox settings, while it may still be shown.
  current(revision) {
    const value = this.snapshot;
    return value && value.revision === revision && this.now() - value.startedAt <= this.keepMs ? value : null;
  }
  // wanted: [{ uuid, createdAt }] of the requests that need placing. Returns
  // the running read to wait for, or null when none is needed or allowed now.
  refresh(revision, config, wanted) {
    if (this.running) return this.running.revision === revision ? this.running.promise : null;
    if (!Array.isArray(wanted) || !wanted.length) return null;
    const now = this.now(), snapshot = this.current(revision);
    if (this.lastStartedAt !== null && now - this.lastStartedAt < this.minIntervalMs) return null;
    if (snapshot && snapshot.errorCode === null && now - snapshot.startedAt < this.maxAgeMs &&
        !wanted.some(({ uuid, createdAt }) => !snapshot.machines.has(uuid) && createdAt > snapshot.startedAt)) return null;
    this.lastStartedAt = now;
    const promise = (async () => {
      let next;
      try {
        if (typeof this.proxmox?.machineIndex !== 'function') throw new HttpError(503, 'proxmox_lookup_unavailable');
        const result = await this.proxmox.machineIndex(config, { signal: AbortSignal.timeout(this.scanMs), maxVms: this.maxVms });
        if (!(result?.machines instanceof Map) || !Number.isSafeInteger(result.unread) || result.unread < 0) throw new HttpError(502, 'proxmox_invalid_response');
        next = { revision, startedAt: now, machines: result.machines, unread: result.unread, errorCode: null };
      } catch (error) {
        next = { revision, startedAt: now, machines: new Map(), unread: 0, errorCode: error instanceof HttpError ? error.code : 'proxmox_unreachable' };
      }
      this.snapshot = next; this.running = null;
    })();
    this.running = { revision, promise };
    return promise;
  }
  // matched: exactly one VM carries the UUID. ambiguous: several do (a restored
  // copy keeps its UUID), so nothing is suggested. not_found: a complete read
  // taken after the request did not contain it. unavailable: Proxmox could not
  // be read (or not completely). searching: no answer yet.
  view(revision, uuid, createdAt) {
    const snapshot = this.current(revision);
    if (!snapshot) return { state: 'searching' };
    const found = snapshot.machines.get(uuid) ?? [];
    if (found.length === 1) return { state: 'matched', vm: { node: found[0].node, vmid: found[0].vmid, name: found[0].name } };
    if (found.length > 1) return { state: 'ambiguous', count: found.length };
    if (snapshot.errorCode !== null) return { state: 'unavailable', errorCode: snapshot.errorCode };
    // The request is newer than the read (its VM may be, too): the next read decides.
    if (createdAt > snapshot.startedAt) return { state: 'searching' };
    return snapshot.unread ? { state: 'unavailable', errorCode: 'proxmox_lookup_incomplete' } : { state: 'not_found' };
  }
}
