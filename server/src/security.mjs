import { randomBytes, scrypt, timingSafeEqual, createHash } from 'node:crypto';
import { promisify } from 'node:util';
import { isIPv4, isIPv6 } from 'node:net';

const derive = promisify(scrypt);
const options = { N: 32768, r: 8, p: 3, maxmem: 64 * 1024 * 1024 };
export function username(value) {
  if (typeof value !== 'string') throw new Error('invalid_username');
  const canonical = value.trim().normalize('NFC').toLowerCase().normalize('NFC');
  if ([...canonical].length < 2 || [...canonical].length > 64 || !/^[\p{L}\p{N}][\p{L}\p{N}\p{M}_.-]*$/u.test(canonical)) throw new Error('invalid_username');
  return canonical;
}
export function notes(value = '') {
  if (typeof value !== 'string' || [...value].length > 500 || /[\x00-\x08\x0b\x0c\x0e-\x1f\x7f]/.test(value)) throw new Error('invalid_notes');
  return value.replace(/\r\n?/g, '\n').trim();
}
export function text(value, max = 100) {
  if (typeof value !== 'string' || !value.trim() || value.trim().length > max || /[\x00-\x1f\x7f]/.test(value)) throw new Error('invalid_text');
  return value.trim();
}
export function desktopId(value) {
  if (typeof value !== 'string' || !/^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$/.test(value)) throw new Error('invalid_desktop_id');
  return value;
}
export async function hashPassword(password) {
  if (typeof password !== 'string' || password.length < 10 || password.length > 256) throw new Error('password_length_10_to_256');
  return encodePassword(password);
}
async function encodePassword(password) {
  const salt = randomBytes(16);
  const key = await derive(password, salt, 32, options);
  return `scrypt-v1$${salt.toString('hex')}$${key.toString('hex')}`;
}
// Fixed bootstrap credential only; normal account creation still requires 10+ characters.
export const initialPasswordHash = () => encodePassword('admin');
export async function verifyPassword(password, encoded) {
  if (typeof password !== 'string' || password.length > 256) return false;
  const parts = encoded.split('$');
  if (parts.length !== 3 || parts[0] !== 'scrypt-v1' || !/^[a-f0-9]{32}$/.test(parts[1]) || !/^[a-f0-9]{64}$/.test(parts[2])) return false;
  const actual = await derive(password, Buffer.from(parts[1], 'hex'), 32, options);
  return timingSafeEqual(actual, Buffer.from(parts[2], 'hex'));
}
export const newToken = () => randomBytes(32).toString('base64url');
export const tokenHash = value => createHash('sha256').update(value).digest('hex');

// Rate-limit identity of a request. Behind the TLS reverse proxy every socket
// comes from the proxy, so per-address limits keyed by the socket were one
// global limit. Only the hop a trusted proxy itself appended to
// X-Forwarded-For is used; anything else in that header is client-supplied.
// IPv6 clients are keyed per /64 (one subscriber's prefix).
function plainAddress(value) {
  if (typeof value !== 'string') return null;
  let address = value.trim();
  if (/^::ffff:\d{1,3}(?:\.\d{1,3}){3}$/i.test(address)) address = address.slice(7);
  return isIPv4(address) || isIPv6(address) ? address.toLowerCase() : null;
}
function limitKey(address) {
  if (isIPv4(address)) return address;
  const bare = address.split('%')[0];
  if (bare.includes('.')) return bare; // embedded IPv4 forms: exact address
  const [head, tail] = bare.includes('::') ? bare.split('::') : [bare, undefined];
  const left = head ? head.split(':') : [], right = tail ? tail.split(':') : [];
  const groups = tail === undefined ? left : [...left, ...Array(8 - left.length - right.length).fill('0'), ...right];
  return groups.slice(0, 4).map(group => parseInt(group, 16).toString(16)).join(':') + '::/64';
}
const privatePeer = address => isIPv4(address)
  ? /^(?:127\.|10\.|192\.168\.|172\.(?:1[6-9]|2\d|3[01])\.)/.test(address)
  : address === '::1' || /^f[cd][0-9a-f]{2}:/.test(address) || /^fe[89ab][0-9a-f]:/.test(address);
// Whose X-Forwarded-For hop names the client for the sign-in and enrolment
// rate limits. The server's access settings give the exact proxy addresses
// (config.mjs passes that list, [] when there is none, from server-access.json
// or MULTITAP_TRUSTED_PROXIES). Nothing given, [] or "none" = never trust the
// header; "private" = one hop from a loopback or private-network peer, never
// a production default.
export function trustedProxies(value) {
  if (Array.isArray(value) && value.length === 0) return { mode: 'none', list: new Set() };
  if (Array.isArray(value)) value = value.join(',');
  if (value === undefined || value === null || value === '') return { mode: 'none', list: new Set() };
  if (typeof value !== 'string') throw new Error('invalid_trusted_proxies');
  if (value.trim().toLowerCase() === 'none') return { mode: 'none', list: new Set() };
  if (value.trim().toLowerCase() === 'private') return { mode: 'private', list: new Set() };
  const list = value.split(',').map(item => plainAddress(item));
  if (!list.length || list.some(item => item === null)) throw new Error('invalid_trusted_proxies');
  return { mode: 'list', list: new Set(list) };
}
export function requestAddress(req, trust = trustedProxies(undefined)) {
  const peer = plainAddress(req.socket?.remoteAddress);
  if (!peer) return 'unknown';
  const trusted = address => trust.mode === 'list' ? trust.list.has(address) : trust.mode === 'private' && privatePeer(address);
  const header = req.headers?.['x-forwarded-for'];
  if (!trusted(peer) || typeof header !== 'string' || header.length > 1024) return limitKey(peer);
  const hops = header.split(',');
  // Private mode trusts exactly one hop (the proxy's own append); an explicit
  // list also walks past further listed proxies.
  for (let index = hops.length - 1; index >= 0; index--) {
    const hop = plainAddress(hops[index]);
    if (!hop) return limitKey(peer);
    if (trust.mode !== 'list' || !trust.list.has(hop)) return limitKey(hop);
  }
  return limitKey(peer);
}
