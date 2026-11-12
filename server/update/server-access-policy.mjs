import { isIP } from 'node:net';
import { isAbsolute, normalize } from 'node:path';
import { createHash } from 'node:crypto';
import { deploymentTransport, normalizeBrokerOrigin } from './transport-policy.mjs';

export const serverAccessProtocol = 'server-access-v1';
export const serverAccessUnconfiguredRevision = createHash('sha256').update('server-access-unconfigured').digest('hex');
const record = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const shape = (value, required, optional = []) => record(value) && required.every(key => Object.hasOwn(value, key)) && Object.keys(value).every(key => [...required, ...optional].includes(key));
const fail = code => { throw Error('server_access_' + code); };
function host(value) {
  if (value === 'localhost') return '127.0.0.1';
  if (typeof value !== 'string' || value.includes('%') || !isIP(value)) fail('listen_host_invalid');
  return value.toLowerCase();
}
function port(value) {
  if (!Number.isInteger(value) || value < 1 || value > 65535) fail('listen_port_invalid');
  return value;
}
function origin(value, scheme, enabled) {
  if (value === null && !enabled) return null;
  let normalized;
  try { normalized = normalizeBrokerOrigin(value, { allowInsecureHttp: scheme === 'http' }); } catch { fail('origin_invalid'); }
  if (!normalized.startsWith(scheme + '://')) fail('origin_invalid');
  return normalized;
}
// access-origins-v1: an endpoint may also answer up to three more
// exact addresses on its own listener, e.g. the public IP a router forwards
// beside the LAN address. Never a wildcard or "any address". Omitted when empty,
// so earlier saved policies keep their revision.
export const maxExtraOrigins = 3;
function extraOrigins(value, scheme, main) {
  if (value === undefined) return [];
  if (!Array.isArray(value) || value.length > maxExtraOrigins || (main === null && value.length)) fail('origin_invalid');
  const list = value.map(item => {
    const normalized = origin(item, scheme, true), hostname = new URL(normalized).hostname.replace(/^\[|\]$/g, '');
    if (['0.0.0.0', '::'].includes(hostname)) fail('origin_invalid');
    return normalized;
  });
  if (new Set([main, ...list]).size !== list.length + 1) fail('origin_invalid');
  return list;
}
const extrasOf = list => list.length ? { extraOrigins: list } : {};
export const endpointOrigins = row => row.origin ? [row.origin, ...(row.extraOrigins ?? [])] : [];
function tlsPath(value) {
  if (typeof value !== 'string' || !isAbsolute(value) || /^[/\\]{2}/.test(value) || value === '/' || normalize(value).replaceAll('\\', '/') !== value.replaceAll('\\', '/') || value.length > 512 || /[\x00-\x20\x7f]/.test(value)) fail('tls_path_invalid');
  return value;
}
export function normalizeServerAccess(value) {
  if (!shape(value, ['protocol', 'primaryOrigin', 'https', 'http', 'trustedProxies']) || value.protocol !== serverAccessProtocol) fail('invalid');
  const s = value.https, p = value.http;
  if (!shape(s, ['enabled', 'origin', 'mode', 'listenHost', 'listenPort'], ['extraOrigins', 'certFile', 'keyFile']) || typeof s.enabled !== 'boolean' || !['proxy', 'direct'].includes(s.mode) ||
      !shape(p, ['enabled', 'origin', 'listenHost', 'listenPort'], ['extraOrigins']) || typeof p.enabled !== 'boolean') fail('invalid');
  const secureOrigin = origin(s.origin, 'https', s.enabled), plainOrigin = origin(p.origin, 'http', p.enabled);
  const https = { enabled: s.enabled, origin: secureOrigin, ...extrasOf(extraOrigins(s.extraOrigins, 'https', secureOrigin)), mode: s.mode, listenHost: host(s.listenHost), listenPort: port(s.listenPort), certFile: s.certFile ?? null, keyFile: s.keyFile ?? null };
  const http = { enabled: p.enabled, origin: plainOrigin, ...extrasOf(extraOrigins(p.extraOrigins, 'http', plainOrigin)), listenHost: host(p.listenHost), listenPort: port(p.listenPort) };
  if (https.mode === 'proxy' && (https.certFile !== null || https.keyFile !== null)) fail('tls_configuration_invalid');
  if (https.mode === 'direct' && (https.enabled || https.certFile !== null || https.keyFile !== null)) {
    https.certFile = tlsPath(https.certFile); https.keyFile = tlsPath(https.keyFile);
    if (https.certFile === https.keyFile) fail('tls_configuration_invalid');
  }
  if (!Array.isArray(value.trustedProxies) || value.trustedProxies.length > 32) fail('trusted_proxies_invalid');
  const trustedProxies = value.trustedProxies.map(value => {
    if (typeof value !== 'string' || value.trim() !== value || value.includes('%') || !isIP(value)) fail('trusted_proxies_invalid');
    // Match the broker's exact peer normalization; never accept a CIDR/private mode.
    return /^::ffff:\d{1,3}(?:\.\d{1,3}){3}$/i.test(value) ? value.slice(7) : value.toLowerCase();
  });
  if (new Set(trustedProxies).size !== trustedProxies.length) fail('trusted_proxies_invalid');
  if (!https.enabled && !http.enabled) fail('last_management_path');
  let primaryOrigin;
  try { primaryOrigin = normalizeBrokerOrigin(value.primaryOrigin, { allowInsecureHttp: http.enabled }); } catch { fail('primary_origin_invalid'); }
  if (![https.enabled && https.origin, http.enabled && http.origin].includes(primaryOrigin)) fail('primary_origin_invalid');
  if (https.enabled && http.enabled && https.listenPort === http.listenPort) {
    const same = https.listenHost === http.listenHost;
    const wildcard = [https.listenHost, http.listenHost].includes('::') || ([https.listenHost, http.listenHost].includes('0.0.0.0') && isIP(https.listenHost) === 4 && isIP(http.listenHost) === 4);
    if ((same || wildcard) && !(same && https.mode === 'proxy')) fail('listener_conflict');
  }
  return { protocol: serverAccessProtocol, primaryOrigin, https, http, trustedProxies };
}
export function allowedServerOrigins(value) {
  const config = normalizeServerAccess(value);
  return [config.primaryOrigin, ...[config.https, config.http].filter(row => row.enabled).flatMap(endpointOrigins).filter(value => value !== config.primaryOrigin)];
}
// The main addresses only: the post-apply public probe. An extra address is
// typically a router forward the server itself may not reach (no hairpin NAT).
export function probedServerOrigins(value) {
  const config = normalizeServerAccess(value);
  return [config.primaryOrigin, ...[config.https, config.http].filter(row => row.enabled).map(row => row.origin).filter(value => value !== config.primaryOrigin)];
}
export function legacyServerAccess(env = {}) {
  const previous = deploymentTransport(env), secure = previous.publicOrigin.startsWith('https:');
  const text = env.MULTITAP_TRUSTED_PROXIES;
  const proxies = text === undefined || text === '' ? ['127.0.0.1', '::1'] : text.trim().toLowerCase() === 'none' ? [] : text.split(',').map(value => value.trim());
  return normalizeServerAccess({ protocol: serverAccessProtocol, primaryOrigin: previous.publicOrigin,
    https: { enabled: secure, origin: secure ? previous.publicOrigin : null, mode: previous.tlsMode, listenHost: previous.host, listenPort: secure ? previous.port : 18443, certFile: previous.tlsFiles?.certFile ?? null, keyFile: previous.tlsFiles?.keyFile ?? null },
    http: { enabled: !secure, origin: secure ? null : previous.publicOrigin, listenHost: previous.host, listenPort: secure ? 18481 : previous.port }, trustedProxies: proxies });
}
export function serverAccessTransport(value) {
  const config = normalizeServerAccess(value), secure = config.primaryOrigin === config.https.origin, primary = secure ? config.https : config.http;
  return { publicOrigin: config.primaryOrigin, allowInsecureHttp: config.http.enabled, tlsMode: secure ? config.https.mode : 'proxy', tlsFiles: secure && config.https.mode === 'direct' ? { certFile: config.https.certFile, keyFile: config.https.keyFile } : null, host: primary.listenHost, port: primary.listenPort };
}
export function serverAccessListeners(value) {
  const config = normalizeServerAccess(value), listeners = [];
  for (const [scheme, row] of [['https', config.https], ['http', config.http]]) if (row.enabled) {
    const protocol = scheme === 'https' && row.mode === 'direct' ? 'https' : 'http';
    let listener = listeners.find(value => value.protocol === protocol && value.host === row.listenHost && value.port === row.listenPort);
    if (!listener) { listener = { protocol, host: row.listenHost, port: row.listenPort, origins: [], tlsFiles: protocol === 'https' ? { certFile: row.certFile, keyFile: row.keyFile } : null }; listeners.push(listener); }
    listener.origins.push(...endpointOrigins(row));
  }
  return listeners;
}
// An external port that differs from its listener needs a hop in between: the
// HTTPS proxy (also for HTTP on the proxy's host name or a loopback listener),
// Docker's published port or a router. Returns endpoints with no known hop;
// the caller refuses those whose address is this machine (no router there).
export function serverAccessPortMismatches(value, { dockerPorts = null } = {}) {
  const config = normalizeServerAccess(value), rows = [];
  const proxyHosts = config.https.enabled && config.https.mode === 'proxy' ? endpointOrigins(config.https).map(value => new URL(value).hostname) : [];
  for (const scheme of ['https', 'http']) {
    const row = config[scheme];
    if (!row.enabled || (scheme === 'https' && row.mode === 'proxy') || ['127.0.0.1', '::1'].includes(row.listenHost)) continue;
    for (const address of endpointOrigins(row)) {
      const url = new URL(address), external = Number(url.port || (scheme === 'https' ? 443 : 80));
      if (proxyHosts.includes(url.hostname)) continue;
      const reachable = dockerPorts?.find(port => port.scheme === scheme)?.hostPort ?? row.listenPort;
      if (external !== reachable) rows.push({ scheme, hostname: url.hostname.replace(/^\[|\]$/g, ''), external, listenPort: row.listenPort });
    }
  }
  return rows;
}
export const serverAccessHash = value => createHash('sha256').update(JSON.stringify(normalizeServerAccess(value))).digest('hex');
export function decodeServerAccessState(value) {
  if (!shape(value, ['schema', 'protocol', 'revision', 'config', 'updatedAt']) || value.schema !== 1 || value.protocol !== serverAccessProtocol || !/^[a-f0-9]{64}$/.test(value.revision ?? '') || typeof value.updatedAt !== 'string' || !Number.isFinite(Date.parse(value.updatedAt))) fail('state_invalid');
  const config = normalizeServerAccess(value.config);
  if (value.revision !== serverAccessHash(config)) fail('state_invalid');
  return { schema: 1, protocol: serverAccessProtocol, revision: value.revision, config, updatedAt: value.updatedAt };
}
