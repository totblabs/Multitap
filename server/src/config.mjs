import { resolve, dirname, isAbsolute } from 'node:path';
import { isIP, isIPv4 } from 'node:net';
import { constants, lstatSync, openSync, fstatSync, readSync, closeSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
import { trustedProxies } from './security.mjs';
import { normalizeBrokerOrigin, deploymentPort, explicitHttpOptIn } from './transport-policy.mjs';
import { parseUdpMediaScope, parseUdpRelays } from './udp-media.mjs';
import { dockerUpdateFeed } from './docker-update-check.mjs';
import { normalizeServerAccess, legacyServerAccess, allowedServerOrigins, serverAccessTransport, decodeServerAccessState, maxExtraOrigins, serverAccessHash } from '../update/server-access-policy.mjs';
import { selfAccessPaths, selfAccessFixed, conformSelfAccess, decodeSelfPolicy, selfPolicyRevision, selfEnvSeed, revertUnloadableSelfPolicy } from './self-access.mjs';

export function validatePublicOrigin(value, options) { return normalizeBrokerOrigin(value, options); }

export function validateDeploymentKind(value = 'unknown') {
  if (!['unknown', 'docker', 'systemd', 'external'].includes(value)) throw Error('invalid_deployment_kind');
  return value;
}

// The UDP media relay stays off unless an administrator sets a port.
// Relay lists (MULTITAP_UDP_HOST_RELAYS, MULTITAP_UDP_CLIENT_RELAYS) name where
// each side sends. A self-managed Docker install with neither list derives
// them (`derivedPort`): each side is given the address it already reaches the
// server at, on the UDP port Docker publishes (MULTITAP_UDP_PUBLISHED_PORT, as
// compose passes it). Not derived while that port is not told or is published
// on loopback only: sessions stay on WebSocket, as with empty lists anywhere.
// Media goes over UDP only where it is turned on: MULTITAP_UDP_MEDIA where it
// is set (`on`, `off` or a list of desktops), else the switch a self-managed
// install saves with its access settings (`saved`), else off. With it off the
// relay still answers, so whether its port is reached can be seen first.
function udpRelayConfiguration(env, selfManaged = false, saved = false) {
  const port = env.MULTITAP_UDP_PORT, host = env.MULTITAP_UDP_HOST || '0.0.0.0';
  if (!port) return null;
  if (!/^[1-9]\d{0,4}$/.test(port) || Number(port) > 65535) throw Error('invalid_udp_port');
  if (!isIPv4(host)) throw Error('invalid_udp_host');
  const stun = env.MULTITAP_STUN_SERVERS === 'off' ? [] : parseUdpRelays(env.MULTITAP_STUN_SERVERS || '');
  if (stun.length > 2) throw Error('invalid_udp_stun');
  // Peer routing is outside the supported relay path. Legacy STUN settings
  // remain syntactically validated but cannot enable it or reach a grant.
  const relays = { host: parseUdpRelays(env.MULTITAP_UDP_HOST_RELAYS), client: parseUdpRelays(env.MULTITAP_UDP_CLIENT_RELAYS) };
  const published = selfManaged && !relays.host.length && !relays.client.length ? /^(?:(\d{1,3}(?:\.\d{1,3}){3}):)?([1-9]\d{0,4})$/.exec(env.MULTITAP_UDP_PUBLISHED_PORT ?? '') : null;
  const derivedPort = published && Number(published[2]) <= 65535 && (published[1] === undefined || (isIPv4(published[1]) && !published[1].startsWith('127.'))) ? Number(published[2]) : null;
  return { port: Number(port), host, allowPeerDirect: false, stun, media: env.MULTITAP_UDP_MEDIA ? parseUdpMediaScope(env.MULTITAP_UDP_MEDIA) : saved === true, relays, ...(derivedPort !== null ? { derivedPort } : {}) };
}
// What .env fixes for media over UDP, as the console names it; null where it says nothing.
const udpMediaEnv = env => !env.MULTITAP_UDP_MEDIA ? null : env.MULTITAP_UDP_MEDIA === 'on' ? 'on' : env.MULTITAP_UDP_MEDIA === 'off' ? 'off' : 'desktops';

// host-route-v1: where Hosts inside this network reach the public origin, so a router
// that loops traffic for its own public address back (hairpin NAT) is skipped. Hosts
// still send and check the public name; unset, they use DNS as before.
function hostRoute(value) {
  if (!value) return null;
  if (!isIPv4(value)) throw Error('invalid_host_route');
  return value;
}

// http-origin-v1 (Docker Compose without an updater): MULTITAP_HTTP_ORIGIN is
// the plain HTTP address for the LAN, beside the HTTPS address on the same
// listener or alone. Like an http:// public origin it needs the explicit
// MULTITAP_ALLOW_INSECURE_HTTP=1. These lines are the policy until one is saved
// from the console or access-cli.mjs (self-access.mjs); a saved policy
// supersedes them. Installs with an updater keep their policy in the console:
// the updater derives the revision from the same environment without this key.
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

// `owner` is the account a production file must belong to: root for the
// files the updater writes, the broker's own account for the ones a
// self-managed Docker install keeps (self-access.mjs).
export function readProtectedAccessFile(path, { maximum = 32768, production = true, owner = 0 } = {}) {
  if (!isAbsolute(path) || !Number.isSafeInteger(maximum) || maximum < 1 || maximum > 262144 || !Number.isSafeInteger(owner) || owner < 0) throw Error('server_access_storage_invalid');
  const uid = BigInt(owner);
  let stat;
  try { stat = lstatSync(path, { bigint: true }); } catch (error) { if (error.code === 'ENOENT') return null; throw Error('server_access_storage_invalid'); }
  if (!stat.isFile() || stat.isSymbolicLink() || stat.size < 1n || stat.size > BigInt(maximum) || (stat.mode & 0o022n) || (production && process.platform !== 'win32' && stat.uid !== uid)) throw Error('server_access_storage_invalid');
  for (let parent = dirname(path); ; parent = dirname(parent)) {
    const directory = lstatSync(parent);
    if (!directory.isDirectory() || directory.isSymbolicLink()) throw Error('server_access_storage_invalid');
    if (dirname(parent) === parent) break;
  }
  let fd;
  try {
    fd = openSync(path, constants.O_RDONLY | (constants.O_NOFOLLOW ?? 0));
    const valid = actual => actual.isFile() && !actual.isSymbolicLink() && actual.dev === stat.dev && actual.ino === stat.ino && actual.size >= 1n && actual.size <= BigInt(maximum) && !(actual.mode & 0o022n) && (!production || process.platform === 'win32' || actual.uid === uid);
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

const dataDirectory = env => resolve(env.MULTITAP_DATA_DIR ?? fileURLToPath(new URL('../../.local/broker', import.meta.url)));
// A Docker Compose install with no updater keeps its policy itself, in the
// data volume (self-access.mjs). An updater's own files always come first.
const selfManagedCandidate = env => env.MULTITAP_DEPLOYMENT_KIND === 'docker' && !env.MULTITAP_UPDATE_SOCKET && !env.MULTITAP_SERVER_ACCESS_FILE;
const selfReader = (env, readAccessFile) => (path, maximum = 32768) => readAccessFile(path, { production: env.NODE_ENV === 'production', owner: process.getuid?.() ?? 0, maximum });
// The .env address lines only seed an install that has saved no policy yet.
// True when somebody changed them after the policy was last saved and they
// name other addresses than it: they are ignored. Lines that are still the
// ones the policy was saved beside (`savedSeed`) are the seed, not an edit.
function ignoredEnvAddresses(env, saved, fixed, savedSeed) {
  const seed = selfEnvSeed(env);
  if (seed === null || seed === savedSeed) return false;
  try {
    const lines = httpOriginSetting(env), listed = value => JSON.stringify([value.primaryOrigin, ...allowedServerOrigins(value).sort()]);
    return listed(conformSelfAccess(withHttpOrigin(legacyServerAccess(lines.env), lines.httpOrigins), fixed)) !== listed(saved.config);
  } catch { return true; }
}
// Where a self-managed install keeps its files, without loading them
// (access-cli.mjs removes a policy that does not load); null for any other install.
export function selfAccessStorage(env = process.env, { readAccessFile = readProtectedAccessFile } = {}) {
  if (!selfManagedCandidate(env) || (env.NODE_ENV === 'production' && !env.MULTITAP_DATA_DIR)) return null;
  return { paths: selfAccessPaths(dataDirectory(env)), read: selfReader(env, readAccessFile) };
}
// index.mjs: a start that cannot load the policy a save has just written.
export async function revertUnloadableSelfAccess(env = process.env, options = {}) {
  const storage = selfAccessStorage(env, options);
  return storage ? revertUnloadableSelfPolicy(storage.paths, storage.read) : false;
}

export function configuration(env = process.env, { readAccessFile = readProtectedAccessFile } = {}) {
  if (env.NODE_ENV === 'production' && !env.MULTITAP_DATA_DIR) throw new Error('MULTITAP_DATA_DIR_required');
  const dataDir = dataDirectory(env);
  let accessFile = env.MULTITAP_SERVER_ACCESS_FILE ?? resolve(dataDir, 'server-access.json');
  let accessIdentityFile = resolve(dataDir, 'server-access.identity');
  const options = { production: env.NODE_ENV === 'production' };
  let bytes = readAccessFile(accessFile, options);
  let self = null;
  if (selfManagedCandidate(env) && bytes === null) {
    const paths = selfAccessPaths(dataDir), read = selfReader(env, readAccessFile), policy = read(paths.policy);
    let fixed = null;
    // A container whose own transport settings are invalid is not managed
    // here: the address settings below report them, unless a policy was saved.
    try { fixed = selfAccessFixed(env); } catch (error) { if (policy !== null) throw error; }
    if (fixed) { self = { paths, fixed, read }; bytes = policy; accessFile = paths.policy; accessIdentityFile = paths.identity; }
  }
  let saved = null, unreadable = null;
  if (bytes !== null && self) {
    try {
      // The container's listener, TLS mode and certificate paths are not the policy's to choose.
      const { config, updatedAt, publicIp, envSeed, udpMedia } = decodeSelfPolicy(bytes, self.fixed);
      const identity = self.read(accessIdentityFile, 128);
      if (identity === null) throw Error('server_access_identity_missing');
      if (!/^[a-f0-9]{64}$/.test(identity.trim())) throw Error('server_access_identity_invalid');
      saved = { schema: 1, protocol: config.protocol, revision: serverAccessHash(config), config, updatedAt }; self.publicIp = publicIp; self.savedEnvSeed = envSeed; self.udpMedia = udpMedia;
    } catch (error) {
      // A saved policy this container cannot use (damaged, written by another
      // version, without its installation ID, or not servable here) does not
      // end the server and is neither replaced nor stood in for by the .env
      // lines: the server starts with no addresses, as before its first ones,
      // so only the administration console answers at the server's own address
      // and says why (`unreadable`). The file stays as it is until an
      // administrator saves again or `access-cli.mjs reset` removes it. A file
      // that cannot be read at all (owner, mode) still ends the start.
      if (!/^server_access_[a-z_]{1,80}$/.test(error?.message ?? '') || error.message === 'server_access_storage_invalid') throw error;
      unreadable = error.message;
    }
  } else if (bytes !== null) {
    try { saved = decodeServerAccessState(JSON.parse(bytes)); } catch { throw Error('server_access_state_invalid'); }
  }
  // The privileged apply writes this explicit file reference into its owned
  // environment only with a saved policy. Its disappearance is not legacy or
  // bootstrap and must not reopen the old unscoped admin-cookie fallback.
  if (env.MULTITAP_SERVER_ACCESS_FILE && !saved) throw Error('server_access_state_missing');
  const deploymentKind = validateDeploymentKind(env.MULTITAP_DEPLOYMENT_KIND);
  // A saved policy supersedes the legacy environment, including its HTTP address.
  const { env: legacyEnv, httpOrigins } = saved || unreadable ? { env, httpOrigins: null } : httpOriginSetting(env);
  const bootstrapOnly = unreadable !== null || (saved === null && env.NODE_ENV === 'production' && !legacyEnv.MULTITAP_PUBLIC_ORIGIN);
  const accessConfig = saved?.config ?? (bootstrapOnly ? null : withHttpOrigin(legacyServerAccess(legacyEnv), httpOrigins));
  const transport = accessConfig ? serverAccessTransport(accessConfig) : { publicOrigin: null, allowInsecureHttp: false, tlsMode: 'proxy', tlsFiles: null, host: '0.0.0.0', port: Number(env.MULTITAP_PORT ?? 18480) };
  if (!Number.isInteger(transport.port) || transport.port < 1 || transport.port > 65535) throw Error('invalid_port');
  const proxies = accessConfig?.trustedProxies ?? [];
  trustedProxies(proxies.length ? proxies : 'none');
  const identityBytes = self ? self.read(accessIdentityFile, 128) : readAccessFile(accessIdentityFile, { ...options, maximum: 128 });
  const identityText = identityBytes?.trim() ?? null;
  // Without a usable policy an installation ID that is not one is not used either.
  const accessInstallationId = unreadable && !/^[a-f0-9]{64}$/.test(identityText ?? '') ? null : identityText;
  if (accessInstallationId !== null && !/^[a-f0-9]{64}$/.test(accessInstallationId)) throw Error('server_access_identity_invalid');
  if (saved && accessInstallationId === null) throw Error('server_access_identity_missing');
  const accessRevision = saved?.revision ?? createHash('sha256').update(accessConfig ? JSON.stringify(normalizeServerAccess(accessConfig)) : 'server-access-unconfigured').digest('hex');
  let legacyPublicOrigin = null;
  if (!saved && !unreadable && legacyEnv.MULTITAP_PUBLIC_ORIGIN) {
    try { legacyPublicOrigin = normalizeBrokerOrigin(legacyEnv.MULTITAP_PUBLIC_ORIGIN, { allowInsecureHttp: env.MULTITAP_ALLOW_INSECURE_HTTP === '1' }); } catch { /* A saved policy supersedes stale legacy settings. */ }
  }
  const udp = udpRelayConfiguration(env, self !== null, self?.udpMedia === true);
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
    // Set for a self-managed Docker install only: where it keeps its policy,
    // what the container fixes, whether it follows the public IP, whether its
    // saved switch lets media go over UDP (`udpMedia`; `udpSwitch`: whether the
    // relay runs here at all, and the value .env fixes instead), the .env
    // address lines of now (`envSeed`) and of the last save (`savedEnvSeed`),
    // and whether lines edited since then are being ignored.
    selfAccess: self ? { ...self, saved: saved !== null, unreadable, publicIp: self.publicIp ?? { follow: false, origin: null }, udpMedia: self.udpMedia === true, udpSwitch: { relay: udp !== null, env: udpMediaEnv(env) },
      revision: selfPolicyRevision(accessConfig, self.publicIp, self.udpMedia === true),
      envSeed: selfEnvSeed(env), savedEnvSeed: self.savedEnvSeed ?? null, envAddressesIgnored: saved !== null && ignoredEnvAddresses(env, saved, self.fixed, self.savedEnvSeed ?? null) } : null,
    // Docker Compose installs without an updater socket: the signed public
    // release feed that docker-update-check.mjs reads (null = no check).
    dockerUpdateFeed: dockerUpdateFeed(env.MULTITAP_DOCKER_UPDATE_FEED),
    udp,
    hostRoute: hostRoute(env.MULTITAP_HOST_ROUTE),
  };
}
