import { isIPv4 } from 'node:net';

export const udpDirectProtocol = 'udp-direct-v1';
export const directCapability = value => value?.udpDirectHostProtocol === udpDirectProtocol;
// Host support and a client request never grant peer routing. Normal broker
// configuration keeps this false; only trusted constructor fixtures opt in.
export const udpPeerDirectAllowed = udp => udp?.allowPeerDirect === true;
export function localCandidate(value) {
  if (!value || value.protocol !== udpDirectProtocol || typeof value.address !== 'string' || !isIPv4(value.address) || !Number.isInteger(value.port) || value.port < 1 || value.port > 65535) return null;
  const [a,b] = value.address.split('.').map(Number);
  if (value.candidateType !== undefined && !['host','srflx'].includes(value.candidateType)) return null;
  const privateAddress = a === 10 || a === 172 && b >= 16 && b <= 31 || a === 192 && b === 168;
  // An authenticated peer can also offer its STUN result. It remains a hint:
  // a fresh cryptographic round trip, not an address claim, admits media.
  if (value.candidateType === 'srflx' ? privateAddress || a === 0 || a === 127 || a >= 224 || a === 169 && b === 254 : !privateAddress) return null;
  return { address: value.address, port: value.port };
}
