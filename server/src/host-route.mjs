import { isIPv4 } from 'node:net';
import { HttpError } from './errors.mjs';

// host-route-v1: the address Hosts inside this server's own network reach it at, so a router that loops traffic
// for its own public address back (hairpin NAT) is skipped. The Broker offers it in host.capabilities.ack (a Host
// resolves the public name to it for its channel, and keeps checking the public name's certificate) and lists it
// first among a Host's UDP relays (udp-relay.mjs relaysFor). A Host that cannot reach it falls back to the public
// name: its channel after a failed attempt, its UDP media because it binds the first relay that answers.
//
// MULTITAP_HOST_ROUTE in the server's .env decides where it is set (config.mjs); otherwise the administrator saves
// it in the console (settings.host_route, PUT /api/v1/admin/host-route). Saved, it applies at once: sessions that
// start from then on list it for the Host, and a Host channel gets it when it next connects.
export const hostRouteSetting = 'host_route';

// An IPv4 address a Host could be sent to, or null for none ('' clears). Not 0.0.0.0/8, loopback, link-local,
// multicast or broadcast: none of those is this server as a Host in its network sees it.
export function hostRouteAddress(value) {
  if (value === null || value === undefined || value === '') return null;
  if (typeof value !== 'string' || !isIPv4(value)) throw new HttpError(400, 'invalid_host_route');
  const [a, b] = value.split('.').map(Number);
  if (a === 0 || a === 127 || a >= 224 || (a === 169 && b === 254)) throw new HttpError(400, 'invalid_host_route');
  return value;
}

export class HostRoute {
  // fixed: the route .env sets (null: none). apply: hands the route in force to the relay.
  constructor({ store, fixed = null, apply = () => {} }) {
    Object.assign(this, { store, fixed: fixed ?? null, apply });
  }
  saved() {
    try { return hostRouteAddress(this.store.setting(hostRouteSetting) ?? null); } catch { return null; }
  }
  current() { return this.fixed ?? this.saved(); }
  describe() { return { fixed: this.fixed, saved: this.saved(), current: this.current() }; }
  save(value, actor) {
    if (this.fixed) throw new HttpError(409, 'host_route_fixed');
    const address = hostRouteAddress(value);
    this.store.saveSetting(hostRouteSetting, address ?? '', actor);
    this.apply(this.current());
    return this.describe();
  }
}
