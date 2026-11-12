import { HttpError } from './errors.mjs';

// host-ready-wait-v1 (restart recovery). A broker update restarts
// the process and ends every stream as server_stopping; each client then waits
// for its desktop's Host to come back to the new broker before it connects
// again. Reading the whole desktop list every half second for that also asked
// Proxmox every 2 s and saw the Host up to half a second late. Instead the
// client asks GET /api/v1/desktops/:id/host-ready: answered the moment that
// Host has reported its capabilities here ({ready:true}), or after holdMs with
// {ready:false} (the client then reads the list once and asks again).
// Memory only; every wait ends (ready:false) when the broker closes, so a held
// request never delays a shutdown. Desktops in the list carry
// hostReadyProtocol; a client that does not find it keeps polling the list.
export const hostReadyProtocol = 'host-ready-wait-v1';

export class HostReadyWaits {
  // reported(desktopId): this broker has the desktop's Host connected and its
  // capabilities in (relay.hostReported).
  constructor({ reported, holdMs = 5000, maxWaits = 256, maxPerUser = 4 }) {
    Object.assign(this, { reported, holdMs, maxWaits, maxPerUser });
    this.waits = new Set(); this.closed = false;
  }
  get size() { return this.waits.size; }
  // True as soon as the Host has reported, false after holdMs or at close.
  // `abandoned`: the request's response; a client that went away ends its wait.
  wait(desktopId, userId, abandoned = null) {
    if (this.closed) return Promise.resolve(false);
    if (this.reported(desktopId)) return Promise.resolve(true);
    let mine = 0;
    for (const wait of this.waits) if (wait.userId === userId) ++mine;
    if (this.waits.size >= this.maxWaits || mine >= this.maxPerUser) throw new HttpError(429, 'too_many_host_ready_waits');
    return new Promise(resolve => {
      const wait = { desktopId, userId, resolve: value => { if (!this.waits.delete(wait)) return; clearTimeout(wait.timer); abandoned?.off?.('close', wait.gone); resolve(value); } };
      wait.timer = setTimeout(() => wait.resolve(false), this.holdMs); wait.timer.unref?.();
      wait.gone = () => wait.resolve(false);
      abandoned?.once?.('close', wait.gone);
      this.waits.add(wait);
    });
  }
  // Something about this desktop's Host changed (Relay.hostEvent).
  wake(desktopId) {
    if (!this.waits.size || !this.reported(desktopId)) return;
    for (const wait of [...this.waits]) if (wait.desktopId === desktopId) wait.resolve(true);
  }
  close() {
    this.closed = true;
    for (const wait of [...this.waits]) wait.resolve(false);
  }
}
