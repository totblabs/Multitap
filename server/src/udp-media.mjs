import { isIPv4 } from 'node:net';

// udp-media-v1, the reachability probe (docs/remote-session-protocol.md,
// "UDP media"). It carries no media and changes no session, so nothing
// here is ever a reason to refuse or end a connection: anything uncertain just
// leaves the session on WebSocket.
export const udpMediaProtocol = 'udp-media-v1';
export const maxUdpRelays = 4;

// Host capability: exactly the marker.
export function udpMediaCapability(value) {
  return value?.udpMediaProtocol === udpMediaProtocol;
}

// Connect body field. A client sends it only after seeing the capability.
export function requestedUdpMedia(mode, protocol) {
  return mode === 'stream' && protocol === udpMediaProtocol;
}

// Independent outputs stay on WebSocket until Phase 4, like every other
// optional stream feature that excludes tracks-v1.
export function negotiateUdpMedia(requested, host, tracks, udp) {
  return requested === true && !tracks && udp?.available === true && udpMediaCapability(host?.capabilities);
}

// MULTITAP_UDP_MEDIA: `on` for every desktop, `off` (or nothing) for none, or a
// comma separated list of desktop ids so a rollout can start with one VM.
export function parseUdpMediaScope(value) {
  if (value === 'on') return true;
  if (value === 'off') return false;
  return value ? new Set(value.split(',')) : false;
}

// Without keys a session's media stays on WebSocket, whatever the legs can reach.
export function udpMediaAllowed(udp, desktopId) {
  return udp?.media === true || (udp?.media instanceof Set && udp.media.has(desktopId));
}

// One leg's message. The ticket is a secret: it goes to this leg alone. `keys`
// tells a client it may send udp.key: a released Broker ends a session over it.
export function udpGrantMessage({ expiresAt }, relays, leg, keys = false, direct = false, stun = [], allowPeerDirect = false) {
  return { type: 'udp.grant', protocol: udpMediaProtocol, relays, id: leg.id.toString('base64'), ticket: leg.ticket.toString('base64'), expiresAt, ...(keys ? { keys: true } : {}), ...(keys && direct && allowPeerDirect === true ? { directProtocol: 'udp-direct-v1', stun } : {}) };
}

// A leg's X25519 public key for Phase 3, passed on unread. The Broker sees it, so
// an active Broker could substitute keys: this is not end-to-end protection.
// Null for anything else; the caller ignores it, because a bad key must not end
// a session that works on WebSocket.
export function udpKeyMessage({ type, protocol, publicKey, sessionId, ...extra }) {
  if (type !== 'udp.key' || protocol !== udpMediaProtocol || Object.keys(extra).length || typeof publicKey !== 'string' || !/^[A-Za-z0-9+/]{43}=$/.test(publicKey) || Buffer.from(publicKey, 'base64').toString('base64') !== publicKey) return null;
  return { type, protocol, publicKey };
}

const label = /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/;
function validRelayHost(host) {
  const labels = host.split('.');
  // A numeric last label can only be an IPv4 address; 1.2.3 is a typo, not a name.
  return host.length <= 253 && labels.every(value => label.test(value)) && (!/^\d+$/.test(labels.at(-1)) || isIPv4(host));
}

// host:port entries, comma separated, in the order peers try them.
export function parseUdpRelays(value) {
  if (!value) return [];
  const entries = value.toLowerCase().split(',');
  if (entries.length > maxUdpRelays || new Set(entries).size !== entries.length) throw Error('invalid_udp_relays');
  return entries.map(entry => {
    const match = /^([a-z0-9.-]+):([1-9]\d{0,4})$/.exec(entry);
    if (!match || !validRelayHost(match[1]) || Number(match[2]) > 65535) throw Error('invalid_udp_relays');
    return { host: match[1], port: Number(match[2]) };
  });
}
