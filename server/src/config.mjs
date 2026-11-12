import { resolve, dirname, isAbsolute } from 'node:path';
import { isIP, isIPv4 } from 'node:net';
import { constants, lstatSync, openSync, fstatSync, readSync, closeSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { trustedProxies } from './security.mjs';
import { normalizeBrokerOrigin, deploymentPort, explicitHttpOptIn } from './transport-policy.mjs';
import { parseUdpMediaScope, parseUdpRelays } from './udp-media.mjs';
import { dockerUpdateFeed } from './docker-update-check.mjs';
import { normalizeServerAccess, legacyServerAccess, allowedServerOrigins, serverAccessTransport, decodeServerAccessState, maxExtraOrigins } from '../update/server-access-policy.mjs';

export function validatePublicOrigin(value, options) { return normalizeBrokerOrigin(value, options); }

export function validateDeploymentKind(value = 'unknown') {
  if (!['unknown', 'docker', 'systemd', 'external'].includes(value)) throw Error('invalid_deployment_kind');
  return value;
}

// The UDP media relay stays off unless an administrator sets a port.
function udpRelayConfiguration(env) {
  const port = env.MULTITAP_UDP_PORT, host = env.MULTITAP_UDP_HOST || '0.0.0.0';
  if (!port) return null;
  if (!/^[1-9]\d{0,4}$/.test(port) || Number(port) > 65535) throw Error('invalid_udp_port');
  if (!isIPv4(host)) throw Error('invalid_udp_host');
  const stun = env.MULTITAP_STUN_SERVERS === 'off' ? [] : parseUdpRelays(env.MULTITAP_STUN_SERVERS || '');
  if (stun.length > 2) throw Error('invalid_udp_stun');
  // Peer routing is outside the supported relay path. Legacy STUN settings
  // remain syntactically validated but cannot enable it or reach a grant.
  return { port: Number(port), host, allowPeerDirect: false, stun, media: parseUdpMediaScope(env.MULTITAP_UDP_MEDIA), relays: { host: parseUdpRelays(env.MULTITAP_UDP_HOST_RELAYS), client: parseUdpRelays(env.MULTITAP_UDP_CLIENT_RELAYS) } };
}

// host-route-v1: where Hosts inside this network reach the public origin, so a router
// that loops traffic for its own public address back (hairpin NAT) is skipped. Hosts
// still send and check the public name; unset, they use DNS as before.
function hostRoute(value) {
  if (!value) return null;
  if (!isIPv4(value)) throw Error('invalid_host_route');
  return value;
}

// http-origin-v1 (Docker Compose, no root updater for the console's server
// access page): MULTITAP_HTTP_ORIGIN is the plain HTTP address for the LAN,
// beside the HTTPS address on the same listener or alone. Like an http://
// public origin it needs the explicit MULTITAP_ALLOW_INSECURE_HTTP=1. Installs
// with an updater keep their policy in the console: the updater derives the
// revision from the same environment without this key.
// A comma-separated list adds exact extra addresses on the same listener,
// e.g. the public IP a router forwards (access-origins-v1).
function httpOriginSetting(env) {
  const value = env.MULTITAP_HTTP_ORIGIN;
  if (!value) return { env, httpOrigins: null };
  if (env.MULTITAP_UPDATE_SOCKET) throw Error('http_origin_use_server_access_settings');
  if (!explicitHttpOptIn(env.MULTITAP_ALLOW_INSECURE_HTTP)) throw Error('http_origin_requires_allow_insecure_http');
  const origins = value.split(',').map(item => item.trim()).filter(Boolean).map(item => {
    let origin;
    try { origin = normalizeBrokerOrigin(item, { allowInsecureHttp: true }); } catch { throw Error('invalid_http_origin'); }
    if (!origin.startsWith('http://')) throw Error('invalid_http_origin');
    return origin;
  });
  if (!origins.length || origins.length > 1 + maxExtraOrigins || new Set(origins).size !== origins.length) throw Error('invalid_http_origin');
  return { env: env.MULTITAP_PUBLIC_ORIGIN ? env : { ...env, MULTITAP_PUBLIC_ORIGIN: origins[0] }, httpOrigins: origins };
}
function withHttpOrigin(config, origins) {
  if (!origins) return config;
  // An http:// public origin must be one of the list; the others are extras.
  if (config.http.enabled) {
    if (!origins.includes(config.http.origin)) throw Error('invalid_http_origin');
    return normalizeServerAccess({ ...config, http: { ...config.http, extraOrigins: origins.filter(value => value !== config.http.origin) } });
  }
  // A TLS listener cannot also answer plain HTTP on its port.
  if (config.https.mode !== 'proxy') throw Error('http_origin_requires_proxy_tls');
  const [origin, ...extraOrigins] = origins;
  return normalizeServerAccess({ ...config, http: { enabled: true, origin, extraOrigins, listenHost: config.https.listenHost, listenPort: config.https.listenPort } });
}

// Docker's first setup by IP. Inside the container the socket's local address
// is the container's own, so the native "server's own IP" bootstrap check
// cannot match. The operator names the server address in .env and compose
// passes the published port: exactly http://<address>:<port> is admitted, still
// only for the bootstrap paths (request-origin.mjs).
function dockerSetupOrigin(env, kind) {
  const value = env.MULTITAP_SERVER_ADDRESS;
  if (!value) return null;
  if (kind !== 'docker') throw Error('server_address_docker_only');
  if (typeof value !== 'string' || value.includes('%') || !isIP(value)) throw Error('invalid_server_address');
  const published = /^(?:(?:\d{1,3}(?:\.\d{1,3}){3}|\[[0-9A-Fa-f:.]+\]):)?([0-9]{1,5})$/.exec(env.MULTITAP_PUBLISHED_PORT ?? '18480');
  if (!published) throw Error('invalid_published_port');
  try { return normalizeBrokerOrigin('http://' + (isIP(value) === 6 ? '[' + value + ']' : value) + ':' + deploymentPort(published[1]), { allowInsecureHttp: true }); }
  catch { throw Error('invalid_server_address'); }
}

export function readProtectedAccessFile(path, { maximum = 32768, production = true } = {}) {
  if (!isAbsolute(path) || !Number.isSafeInteger(maximum) || maximum < 1 || maximum > 262144) throw Error('server_access_storage_invalid');
  let stat;
  try { stat = lstatSync(path, { bigint: true }); } catch (error) { if (error.code === 'ENOENT') return null; throw Error('server_access_storage_invalid'); }
  if (!stat.isFile() || stat.isSymbolicLink() || stat.size < 1n || stat.size > BigInt(maximum) || (stat.mode & 0o022n) || (production && process.platform !== 'win32' && stat.uid !== 0n)) throw Error('server_access_storage_invalid');
  for (let parent = dirname(path); ; parent = dirname(parent)) {
    const directory = lstatSync(parent);
    if (!directory.isDirectory() || directory.isSymbolicLink()) throw Error('server_access_storage_invalid');
    if (dirname(parent) === parent) break;
  }
  let fd;
  try {
    fd = openSync(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    const valid = actual => actual.isFile() && !actual.isSymbolicLink() && actual.dev === stat.dev && actual.ino === stat.ino && actual.size >= 1n && actual.size <= BigInt(maximum) && !(actual.mode & 0o022n) && (!production || process.platform === 'win32' || actual.uid === 0n);
    if (!valid(fstatSync(fd, { bigint: true }))) throw Error('server_access_storage_invalid');
    // Read only this validated descriptor and at most maximum+1 bytes. A
    // rename/replacement between lstat and open cannot substitute another
    // inode; an in-place growth cannot turn this into an unbounded allocation.
    const bytes = Buffer.alloc(maximum + 1); let count = 0;
    while (count < bytes.length) { const read = readSync(fd, bytes, count, bytes.length - count, null); if (!read) break; count += read; }
    if (!count || count > maximum || !valid(fstatSync(fd, { bigint: true }))) throw Error('server_access_storage_invalid');
    return new TextDecoder('utf-8', { fatal: true }).decode(bytes.subarray(0, count));
  } catch { throw Error('server_access_storage_invalid'); }
  finally { if (fd !== undefined) closeSync(fd); }
}

export function configuration(env = process.env, { readAccessFile = readProtectedAccessFile } = {}) {
  if (env.NODE_ENV === 'production' && !env.MULTITAP_DATA_DIR) throw new Error('MULTITAP_DATA_DIR_required');
  const dataDir = resolve(env.MULTITAP_DATA_DIR ?? fileURLToPath(new URL('../../.local/broker', import.meta.url)));
  const accessFile = env.MULTITAP_SERVER_ACCESS_FILE ?? resolve(dataDir, 'server-access.json');
  const accessIdentityFile = resolve(dataDir, 'server-access.identity');
  const options = { production: env.NODE_ENV === 'production' };
  const bytes = readAccessFile(accessFile, options);
  let saved = null;
  if (bytes !== null) {
    try { saved = decodeServerAccessState(JSON.parse(bytes)); } catch { throw Error('server_access_state_invalid'); }
  }
  // The privileged apply writes this explicit file reference into its owned
  // environment only with a saved policy. Its disappearance is not legacy or
  // bootstrap and must not reopen the old unscoped admin-cookie fallback.
  if (env.MULTITAP_SERVER_ACCESS_FILE && !saved) throw Error('server_access_state_missing');
  const deploymentKind = validateDeploymentKind(env.MULTITAP_DEPLOYMENT_KIND);
  // A saved policy supersedes the legacy environment, including its HTTP address.
  const { env: legacyEnv, httpOrigins } = saved ? { env, httpOrigins: null } : httpOriginSetting(env);
  const bootstrapOnly = saved === null && env.NODE_ENV === 'production' && !legacyEnv.MULTITAP_PUBLIC_ORIGIN;
  const accessConfig = saved?.config ?? (bootstrapOnly ? null : withHttpOrigin(legacyServerAccess(legacyEnv), httpOrigins));
  const transport = accessConfig ? serverAccessTransport(accessConfig) : { publicOrigin: null, allowInsecureHttp: false, tlsMode: 'proxy', tlsFiles: null, host: '0.0.0.0', port: Number(env.MULTITAP_PORT ?? 18480) };
  if (!Number.isInteger(transport.port) || transport.port < 1 || transport.port > 65535) throw Error('invalid_port');
  const proxies = accessConfig?.trustedProxies ?? [];
  trustedProxies(proxies.length ? proxies : 'none');
  const identityBytes = readAccessFile(accessIdentityFile, { ...options, maximum: 128 });
  const accessInstallationId = identityBytes?.trim() ?? null;
  if (accessInstallationId !== null && !/^[a-f0-9]{64}$/.test(accessInstallationId)) throw Error('server_access_identity_invalid');
  if (saved && accessInstallationId === null) throw Error('server_access_identity_missing');
  const accessRevision = saved?.revision ?? createHash('sha256').update(accessConfig ? JSON.stringify(normalizeServerAccess(accessConfig)) : 'server-access-unconfigured').digest('hex');
  let legacyPublicOrigin = null;
  if (!saved && legacyEnv.MULTITAP_PUBLIC_ORIGIN) {
    try { legacyPublicOrigin = normalizeBrokerOrigin(legacyEnv.MULTITAP_PUBLIC_ORIGIN, { allowInsecureHttp: env.MULTITAP_ALLOW_INSECURE_HTTP === '1' }); } catch { /* A saved policy supersedes stale legacy settings. */ }
  }
  const accessOperationId = env.MULTITAP_ACCESS_OPERATION_ID || null;
  if (accessOperationId !== null && !/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/.test(accessOperationId)) throw Error('server_access_operation_invalid');
  return {
    trustedProxies: proxies,
    ...transport,
    accessConfig, accessRevision, accessInstallationId, accessFile, accessIdentityFile, accessOperationId, legacyPublicOrigin,
    allowedOrigins: accessConfig ? allowedServerOrigins(accessConfig) : [],
    bootstrapOnly, bootstrapHost: '0.0.0.0', bootstrapPort: transport.port,
    setupOrigin: dockerSetupOrigin(env, deploymentKind),
    legacyProxyTrustNeedsConfiguration: !saved && !bootstrapOnly && accessConfig.https.enabled && accessConfig.https.mode === 'proxy' && !env.MULTITAP_TRUSTED_PROXIES,
    dataDir,
    deploymentKind,
    updateSocket: env.MULTITAP_UPDATE_SOCKET || null,
    // Docker Compose installs without an updater socket: the signed public
    // release feed that docker-update-check.mjs reads (null = no check).
    dockerUpdateFeed: dockerUpdateFeed(env.MULTITAP_DOCKER_UPDATE_FEED),
    udp: udpRelayConfiguration(env),
    hostRoute: hostRoute(env.MULTITAP_HOST_ROUTE),
  };
}
