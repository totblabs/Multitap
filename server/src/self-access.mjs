// Server access settings of a Docker Compose install that has no updater.
// The broker keeps the policy in its own data volume and applies a saved
// change itself: it writes the new policy, asks to be started again (the
// entrypoint restarts it on selfAccessRestartCode), and the new process
// reaches each main address before it keeps the change. An address that does
// not answer as this installation puts the previous policy back.
//
// Files, all in <data>/access and owned by the broker's own account:
//   policy.json            the saved policy: the shape of server-access.json,
//                          plus `publicIp` while the public IP is followed,
//                          `udpMedia` while picture and sound may go over UDP,
//                          and `envSeed`, the .env address lines it was saved beside
//   identity               this installation's ID, made with the first policy
//   journal.json           the save in flight, then its result
//   policy.before-cli.json what `access-cli.mjs revert` puts back
// The listener, the TLS mode, the certificate paths and the published port
// belong to the container (compose and .env). A policy is always brought to
// them, so the two cannot disagree.
//
// Following the public IP (`publicIp.follow`): the broker asks an address echo
// for its public IPv4 every ten minutes and keeps http://<that IP>:<published
// port> as the last extra HTTP address, replacing it when the IP changes. It
// asks only while following is on, and an answer it cannot use changes nothing.
//
// Picture and sound over UDP (`udpMedia`): off unless an administrator turns it
// on, here or with MULTITAP_UDP_MEDIA in .env, which wins where it is set
// (config.mjs). The relay runs and measures whether its port is reached either
// way; the switch only decides whether a session is given keys to send media
// through it. It does not touch the addresses, so saving it restarts nothing
// and disconnects nobody: the running server takes the new value at once
// (hooks.udpMedia), for sessions that connect from then on.
//
// 연결 테스트 reaches the addresses before anything is saved: for each main
// address of the change the server asks itself, at that address, for a
// one-time value (answerProbe below answers it, judged by the policy that
// would apply). Only the server's own answer counts, and it is the same road
// the check after the restart takes. A change whose main address does not lead
// here is therefore refused at the test, with the reason, instead of ending
// everyone's connection first and going back afterwards.
//
// An address that works from outside can still be out of this server's own
// reach: a router that does not loop its public address back inside, or a name
// only the outside resolves. For the reasons that look like that from here
// (no answer, nothing listening, no such name, could not be tried) the change
// can be saved with the administrator's own proof instead (saveOutside): for a
// fixed window the address under proof leads to the console and nothing else
// (provisionalAccess, judged by the policy being saved), and a signed-in
// administrator's request let in at that address, carrying the code shown where
// the save was made, is the proof (confirmOutside). Until then nothing is
// written, fenced or ended; the policy in use stays in use and every way in
// stays open. With the proof it is the save any other is. Without it in time,
// or after a restart of the server in between, nothing has changed. A reason
// that shows a real fault (a certificate, another server answering, a proxy
// this server does not trust) is never offered this way.
//
// The files may also change under a running server: access-cli.mjs writes them
// from another container. The server looks again every few seconds
// (reconcile): what does not touch the addresses in use (whether the public IP
// is followed, the installation ID made with a first policy) it takes over as
// it is, and for other addresses it ends the connections and starts again, so
// the running server and the volume never stay apart.
import { isIP, isIPv4 } from 'node:net';
import { join, dirname } from 'node:path';
import { open, rename, unlink, mkdir } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import { request as httpRequest } from 'node:http';
import { request as httpsRequest } from 'node:https';
import { randomBytes, randomUUID, randomInt, createHash, timingSafeEqual } from 'node:crypto';
import { HttpError } from './errors.mjs';
import { deploymentTransport, normalizeBrokerOrigin } from './transport-policy.mjs';
import { resolveRequestAccess } from './request-origin.mjs';
import { normalizeServerAccess, serverAccessHash, serverAccessProtocol, serverAccessUnconfiguredRevision, decodeServerAccessState, allowedServerOrigins, probedServerOrigins, endpointOrigins, maxExtraOrigins } from '../update/server-access-policy.mjs';
import { serverAccessQuiescence } from '../update/server-access-engine.mjs';
import { publicAccessRequest, validAccessHealth } from '../update/server-access-health.mjs';

// The broker exits with this code to be started again in the same container.
export const selfAccessRestartCode = 75;
export const selfAccessActiveStates = new Set(['awaiting_confirmation', 'waiting_idle', 'applying', 'health_check', 'rolling_back']);
// How long a save waits for the administrator's own proof of an address.
export const outsideConfirmationMs = 300000;
// What an address that works from outside can look like from inside. Every
// other reason is something answering wrongly, which no proof from outside mends.
const outsideReasons = new Set(['server_access_address_no_answer', 'server_access_address_closed', 'server_access_address_name_unknown', 'server_access_address_unchecked']);
const uuid = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/, hex = /^[a-f0-9]{64}$/;
const journalMaximum = 131072;
// The listen port a disabled endpoint carries, as the .env settings give it.
const idlePorts = Object.freeze({ https: 18443, http: 18481 });
const notFollowing = Object.freeze({ follow: false, origin: null });
const record = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const exact = (value, keys) => record(value) && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const fail = code => { throw Error('server_access_' + code); };
const codeOf = error => error instanceof HttpError ? error.code : /^server_access_[a-z_]{1,80}$/.test(error?.message) ? error.message : 'server_access_failed';
// The answers the updater's control socket gives for the same codes.
const statusOf = code => /_(busy|revision_changed|test_required|fence_lost|management_origin_changed|operation_reused|cancel_unavailable|cancel_too_late|confirmation_unavailable|confirmation_origin|confirmation_code)$/.test(code) ? 409
  : /_(invalid|required|conflict)$/.test(code) && !/_storage_invalid$/.test(code) ? 400 : 503;
const httpError = error => { const code = codeOf(error); return new HttpError(statusOf(code), code); };
const enabledOrigins = row => row.enabled ? endpointOrigins(row) : [];

export function selfAccessPaths(dataDir) {
  const directory = join(dataDir, 'access');
  return { directory, policy: join(directory, 'policy.json'), identity: join(directory, 'identity'), journal: join(directory, 'journal.json'), beforeCli: join(directory, 'policy.before-cli.json') };
}

// What the container fixes. `published` is the host side of the listener as
// compose passes it (MULTITAP_PUBLISHED_PORT), null when it is not told.
export function selfAccessFixed(env) {
  const transport = deploymentTransport({ MULTITAP_HOST: env.MULTITAP_HOST, MULTITAP_PORT: env.MULTITAP_PORT, MULTITAP_TLS_MODE: env.MULTITAP_TLS_MODE, MULTITAP_TLS_CERT_FILE: env.MULTITAP_TLS_CERT_FILE, MULTITAP_TLS_KEY_FILE: env.MULTITAP_TLS_KEY_FILE });
  const match = /^(?:(\d{1,3}(?:\.\d{1,3}){3}|\[[0-9A-Fa-f:.]+\]):)?([0-9]{1,5})$/.exec(env.MULTITAP_PUBLISHED_PORT ?? '');
  const port = match ? Number(match[2]) : 0;
  const address = env.MULTITAP_SERVER_ADDRESS;
  return { listenHost: transport.host, listenPort: transport.port, httpsMode: transport.tlsMode, certFile: transport.tlsFiles?.certFile ?? null, keyFile: transport.tlsFiles?.keyFile ?? null, idlePorts,
    published: port >= 1 && port <= 65535 ? { host: match[1]?.replace(/^\[|\]$/g, '').toLowerCase() ?? null, port } : null,
    serverAddress: typeof address === 'string' && !address.includes('%') && isIP(address) ? address.toLowerCase() : null };
}
const loopbackPublished = fixed => ['127.0.0.1', '::1'].includes(fixed.published?.host);
// The IPv4 gateway of the container's default route, from the kernel's own
// table (/proc/net/route: readable by any account, also on a read-only root).
// On Docker's bridge network a reverse proxy that runs on the Docker host
// arrives from this address. Null where there is no such file or no such route.
export function defaultGateway(read = process.platform === 'linux' ? () => readFileSync('/proc/net/route', 'latin1') : null) {
  if (!read) return null;
  try {
    for (const line of read().split('\n').slice(1)) {
      const fields = line.trim().split(/\s+/);
      // Destination and mask 0.0.0.0, flags up (1) and gateway (2); addresses are little-endian hex.
      if (fields.length < 8 || fields[1] !== '00000000' || fields[7] !== '00000000' || !/^[0-9A-Fa-f]{8}$/.test(fields[2]) || (parseInt(fields[3], 16) & 3) !== 3) continue;
      const address = fields[2].match(/../g).reverse().map(byte => parseInt(byte, 16)).join('.');
      return address === '0.0.0.0' ? null : address;
    }
  } catch { /* no table to read */ }
  return null;
}

// The policy for these addresses on this container's listener. The primary
// address leads its own list.
export function selfAccessPolicy({ primaryOrigin, https = [], http = [], trustedProxies = [] }, fixed) {
  const direct = fixed.httpsMode === 'direct';
  // One listener, one protocol: the broker's own TLS serves no HTTP beside it.
  if (direct && http.length) fail('direct_tls_http_conflict');
  const endpoint = (list, kind) => {
    const at = list.indexOf(primaryOrigin), ordered = at > 0 ? [list[at], ...list.slice(0, at), ...list.slice(at + 1)] : list;
    return { enabled: ordered.length > 0, origin: ordered[0] ?? null, ...(ordered.length > 1 ? { extraOrigins: ordered.slice(1) } : {}), listenHost: fixed.listenHost, listenPort: ordered.length ? fixed.listenPort : fixed.idlePorts[kind] };
  };
  return normalizeServerAccess({ protocol: serverAccessProtocol, primaryOrigin,
    https: { ...endpoint(https, 'https'), mode: fixed.httpsMode, certFile: direct ? fixed.certFile : null, keyFile: direct ? fixed.keyFile : null },
    http: endpoint(http, 'http'), trustedProxies });
}
// Any valid policy brought to this container: its addresses, primary address
// and trusted proxies stay; listener, TLS mode and certificate paths are fixed.
export function conformSelfAccess(value, fixed) {
  const config = normalizeServerAccess(value);
  return selfAccessPolicy({ primaryOrigin: config.primaryOrigin, https: enabledOrigins(config.https), http: enabledOrigins(config.http), trustedProxies: config.trustedProxies }, fixed);
}
// What this container cannot serve, for the addresses a change adds:
// - HTTP on the server's own address at another port than the published one;
// - HTTP for the network while the port is published on loopback only.
export function selfAccessRules(config, fixed, current = null) {
  if (!config.http.enabled || !fixed.published) return;
  const known = current ? enabledOrigins(current.http) : [];
  for (const value of endpointOrigins(config.http)) {
    if (known.includes(value)) continue;
    const url = new URL(value), hostname = url.hostname.replace(/^\[|\]$/g, ''), loopback = hostname === 'localhost' || hostname === '::1' || /^127\./.test(hostname);
    if (loopbackPublished(fixed) && !loopback) fail('published_loopback_conflict');
    if (hostname === fixed.serverAddress && Number(url.port || 80) !== fixed.published.port) fail('published_port_conflict');
  }
}

// ---- following the public IP ------------------------------------------------
// Where it can work: HTTP is in use and its port is published beyond loopback.
export const publicIpOffered = (config, fixed) => config?.http.enabled === true && fixed.httpsMode === 'proxy' && fixed.published !== null && !loopbackPublished(fixed);
// The followed address is the last extra HTTP address: never the HTTP main
// address (that one is reached after every restart) and never the primary.
export function followedOrigin(config, publicIp) {
  if (!publicIp.follow || !publicIp.origin || !config) return null;
  const list = enabledOrigins(config.http);
  return list.length > 1 && list.at(-1) === publicIp.origin && config.primaryOrigin !== publicIp.origin ? publicIp.origin : null;
}
function decodeFollow(value, config) {
  if (value === undefined) return notFollowing;
  if (!exact(value, ['follow', 'origin']) || typeof value.follow !== 'boolean' || !(value.origin === null || typeof value.origin === 'string')) fail('state_invalid');
  return value.follow ? { follow: true, origin: followedOrigin(config, value) } : notFollowing;
}
// A public IPv4 as the address echo gives it; null for anything else,
// private, shared (100.64/10), link-local and loopback ranges included.
export function publicIpv4(value) {
  if (typeof value !== 'string' || !isIPv4(value)) return null;
  const [a, b] = value.split('.').map(Number);
  return a === 10 || a === 127 || a === 0 || (a === 172 && b >= 16 && b <= 31) || (a === 192 && b === 168) || (a === 100 && b >= 64 && b <= 127) || (a === 169 && b === 254) ? null : value;
}
export async function lookupPublicIp({ fetcher = fetch, timeoutMs = 5000 } = {}) {
  try {
    const response = await fetcher('https://1.1.1.1/cdn-cgi/trace', { redirect: 'error', signal: AbortSignal.timeout(timeoutMs), headers: { Accept: 'text/plain' } });
    if (!response.ok) return null;
    return publicIpv4(/^ip=(.*)$/m.exec((await response.text()).slice(0, 4096))?.[1]?.trim());
  } catch { return null; }
}

// The address a request would be let in at under `config`, as http.mjs judges it.
const admittedOrigin = (admitted, config) => { try { const origin = typeof admitted === 'function' ? admitted(config) : null; return typeof origin === 'string' ? origin : null; } catch { return null; } };

// ---- reaching an address before it is saved ---------------------------------
export const accessProbeProtocol = 'server-access-probe-v1';
export const accessProbePath = nonce => '/api/v1/access-probe/' + nonce;
// One GET of the probe path at `origin`: no redirect, cookie or certificate
// exception. Resolves { answer } with the JSON that came back (null when it
// was none), or { error } with the reason nothing came back:
//   name_unknown  the name has no address
//   unchecked     this server could not try (its name lookup or network failed)
//   closed        nothing accepts a connection there
//   no_answer     no answer in time
//   not_https     an https address that does not speak TLS
//   certificate   the certificate is not valid for the name
export function probeAddress(origin, nonce, { http = httpRequest, https = httpsRequest, timeoutMs = 4000 } = {}) {
  return new Promise(resolve => {
    let settled = false, timer, request;
    const finish = value => { if (settled) return; settled = true; clearTimeout(timer); resolve(value); };
    const failed = error => {
      const code = String(error?.code ?? '');
      finish({ error: ['ENOTFOUND', 'EAI_NONAME', 'EAI_NODATA'].includes(code) ? 'name_unknown'
        : ['EAI_AGAIN', 'EAI_FAIL', 'ENETUNREACH', 'ENETDOWN', 'EADDRNOTAVAIL'].includes(code) ? 'unchecked'
        : ['ECONNREFUSED', 'EHOSTUNREACH', 'ECONNRESET', 'EPIPE'].includes(code) ? 'closed'
        : code === 'EPROTO' || /^ERR_SSL_/.test(code) ? 'not_https'
        : /^(?:CERT_|ERR_TLS_|DEPTH_ZERO_|SELF_SIGNED_|UNABLE_TO_|HOSTNAME_MISMATCH)/.test(code) ? 'certificate' : 'no_answer' });
    };
    try {
      const target = new URL(accessProbePath(nonce), origin), secure = target.protocol === 'https:';
      request = (secure ? https : http)({ protocol: target.protocol, hostname: target.hostname.replace(/^\[|\]$/g, ''), port: Number(target.port || (secure ? 443 : 80)), path: target.pathname, method: 'GET', timeout: timeoutMs - 500,
        headers: { Host: target.host, Accept: 'application/json', 'User-Agent': 'Multitap-Access-Verification/1' }, ...(secure ? { rejectUnauthorized: true } : {}) }, response => {
        let size = 0; const chunks = [];
        response.on('data', chunk => { size += chunk.length; if (size > 4096) { response.destroy(); finish({ answer: null }); } else chunks.push(chunk); });
        response.once('error', () => finish({ answer: null })); response.once('aborted', () => finish({ answer: null }));
        response.once('end', () => { let answer = null; if (response.statusCode === 200) { try { answer = JSON.parse(Buffer.concat(chunks).toString('utf8')); } catch { answer = null; } } finish({ answer }); });
      });
      timer = setTimeout(() => { request.destroy(); finish({ error: 'no_answer' }); }, timeoutMs); timer.unref?.();
      request.once('error', failed); request.once('timeout', () => { request.destroy(); finish({ error: 'no_answer' }); }); request.end();
    } catch { finish({ error: 'no_answer' }); }
  });
}

// ---- the files -------------------------------------------------------------
async function writeText(path, text) {
  await mkdir(dirname(path), { recursive: true, mode: 0o700 });
  const temporary = path + '.' + randomBytes(8).toString('hex') + '.tmp';
  const file = await open(temporary, 'wx', 0o600);
  try { await file.writeFile(text); await file.sync(); } finally { await file.close(); }
  await rename(temporary, path);
  if (process.platform === 'linux') { const directory = await open(dirname(path), 'r'); try { await directory.sync(); } finally { await directory.close(); } }
}
async function remove(path) { try { await unlink(path); } catch (error) { if (error.code !== 'ENOENT') throw error; } }
// The revision the console and the journal compare: the policy's own hash,
// and another value while the public IP is followed.
export function selfPolicyRevision(config, publicIp = notFollowing, udpMedia = false) {
  const base = config ? serverAccessHash(config) : serverAccessUnconfiguredRevision;
  const followed = publicIp.follow ? createHash('sha256').update(base + ':public-ip:' + (publicIp.origin ?? '')).digest('hex') : base;
  return udpMedia ? createHash('sha256').update(followed + ':udp-media').digest('hex') : followed;
}
// The .env address lines as one value, null when there are none. A policy
// keeps the value an administrator's save was made beside: lines that are
// still those were never meant to change the saved policy, while lines edited
// afterwards were (config.mjs warns that they are ignored).
export function selfEnvSeed(env) {
  const lines = [env.MULTITAP_PUBLIC_ORIGIN ?? '', env.MULTITAP_HTTP_ORIGIN ?? ''];
  return lines.some(Boolean) ? createHash('sha256').update(JSON.stringify(lines)).digest('hex') : null;
}
export function selfPolicyText(config, publicIp = notFollowing, now = Date.now, envSeed = null, udpMedia = false) {
  const normalized = normalizeServerAccess(config);
  return JSON.stringify({ schema: 1, protocol: serverAccessProtocol, revision: serverAccessHash(normalized), config: normalized, updatedAt: new Date(now()).toISOString(),
    ...(publicIp.follow ? { publicIp: { follow: true, origin: followedOrigin(normalized, publicIp) } } : {}), ...(udpMedia ? { udpMedia: true } : {}), ...(envSeed ? { envSeed } : {}) });
}
// A policy file as this container runs it.
export function decodeSelfPolicy(text, fixed) {
  let state, follow, seed, udp;
  try {
    const { publicIp, envSeed = null, udpMedia = false, ...rest } = JSON.parse(text);
    if ((envSeed !== null && !hex.test(envSeed)) || typeof udpMedia !== 'boolean') throw Error();
    state = decodeServerAccessState(rest); follow = publicIp; seed = envSeed; udp = udpMedia;
  } catch { fail('state_invalid'); }
  const config = conformSelfAccess(state.config, fixed);
  return { config, updatedAt: state.updatedAt, publicIp: decodeFollow(follow, config), envSeed: seed, udpMedia: udp };
}
// `text` is a policy file's exact content; null means no saved policy.
export async function putSelfPolicy(paths, text) { if (text === null) await remove(paths.policy); else await writeText(paths.policy, text); }
export async function ensureSelfIdentity(paths, read) {
  const existing = read(paths.identity, 128);
  if (existing !== null) { if (!hex.test(existing.trim())) fail('identity_invalid'); return existing.trim(); }
  const value = randomBytes(32).toString('hex');
  await writeText(paths.identity, value + '\n');
  return value;
}
export function readSelfJournal(paths, read) {
  const text = read(paths.journal, journalMaximum);
  if (text === null) return null;
  let value;
  try { value = JSON.parse(text); } catch { fail('storage_invalid'); }
  if (!record(value) || value.schema !== 1 || !uuid.test(value.operationId ?? '') || typeof value.state !== 'string' || !hex.test(value.revision ?? '') || !hex.test(value.previousRevision ?? '') ||
    !(value.previous === null || value.previous === undefined || typeof value.previous === 'string')) fail('storage_invalid');
  return value;
}
export const clearSelfJournal = paths => remove(paths.journal);
export const removeSelfIdentity = paths => remove(paths.identity);
// The copy `access-cli.mjs revert` puts back; `previous` null means there was none.
export const writeSelfBeforeCli = (paths, previous) => writeText(paths.beforeCli, JSON.stringify({ schema: 1, previous }));
export function readSelfBeforeCli(paths, read) {
  const text = read(paths.beforeCli, 65536);
  if (text === null) return null;
  let value;
  try { value = JSON.parse(text); } catch { fail('storage_invalid'); }
  if (!exact(value, ['schema', 'previous']) || value.schema !== 1 || !(value.previous === null || typeof value.previous === 'string')) fail('storage_invalid');
  return value;
}
export const clearSelfBeforeCli = paths => remove(paths.beforeCli);

// A start that cannot load the policy a save has just written puts the
// previous one back. True when it did: the caller asks to be started again.
export async function revertUnloadableSelfPolicy(paths, read) {
  let journal;
  try { journal = readSelfJournal(paths, read); } catch { return false; }
  if (!journal || !['applying', 'health_check'].includes(journal.state)) return false;
  await writeText(paths.journal, JSON.stringify({ ...journal, state: 'rolling_back', errorCode: 'server_access_start_failed' }));
  await putSelfPolicy(paths, journal.previous ?? null);
  return true;
}

// A change as this container would run it: the policy, whether the public IP
// is followed, whether media may go over UDP (as it is now, unless said), and
// the revision of them all. `current` is what runs now; its
// registered public address, when the new list still has it, stays the last
// extra HTTP address.
export function selfAccessCandidate(value, follow, { fixed, current }, udpMedia = current.udpMedia === true) {
  if (fixed.httpsMode === 'direct' && value?.http?.enabled === true) fail('direct_tls_http_conflict');
  let config = conformSelfAccess(value, fixed), origin = null;
  selfAccessRules(config, fixed, current.config);
  if (follow) {
    if (!publicIpOffered(config, fixed)) fail('public_ip_conflict');
    const list = endpointOrigins(config.http), known = current.publicIp.origin;
    if (known && list.includes(known)) {
      if (list.length < 2 || config.primaryOrigin === known) fail('public_ip_conflict');
      config = selfAccessPolicy({ primaryOrigin: config.primaryOrigin, https: enabledOrigins(config.https), http: [...list.filter(item => item !== known), known], trustedProxies: config.trustedProxies }, fixed);
      origin = known;
    }
  }
  const publicIp = follow ? { follow: true, origin } : notFollowing;
  return { config, publicIp, udpMedia, revision: selfPolicyRevision(config, publicIp, udpMedia) };
}

// The policy as the console edits it: the HTTP section says whether the
// public IP is followed, and `udpMedia` whether media may go over UDP. Each is
// there only while it is on.
const shown = (config, publicIp, udpMedia = false) => !config ? null
  : { ...(publicIp?.follow ? { ...config, http: { ...config.http, followPublicIp: true } } : config), ...(udpMedia ? { udpMedia: true } : {}) };
// `udpMedia` is undefined where the console did not say: like the follow flag, a switch that is not there is off.
function splitFollow(value) {
  if (!record(value)) return { config: value, follow: false };
  let { udpMedia, ...config } = value, follow = false;
  if (udpMedia !== undefined && typeof udpMedia !== 'boolean') fail('invalid');
  if (record(config.http) && Object.hasOwn(config.http, 'followPublicIp')) {
    const { followPublicIp, ...http } = config.http;
    if (typeof followPublicIp !== 'boolean') fail('invalid');
    config = { ...config, http }; follow = followPublicIp;
  }
  return { config, follow, udpMedia };
}

// The same four calls the updater's control socket answers
// (server-access-client.mjs), made in this process. AccessRuntime holds the
// admission fence around them exactly as it does for the updater.
export class SelfAccess {
  // `configuration` is config.mjs's result for a self-managed install.
  // What a process runs, from what config.mjs loaded.
  static currentOf(configuration) {
    const { saved, revision, publicIp, udpMedia } = configuration.selfAccess;
    return { config: configuration.accessConfig, healthRevision: configuration.accessRevision, revision, saved, publicIp, udpMedia: udpMedia === true, installationId: configuration.accessInstallationId };
  }
  // `load` loads the configuration again (reconcile).
  static from(configuration, options) {
    const { paths, fixed, read, envAddressesIgnored, envSeed, savedEnvSeed, unreadable, udpSwitch } = configuration.selfAccess;
    return new SelfAccess({ paths, fixed, read, envAddressesIgnored, envSeed, savedEnvSeed, unreadable, udpSwitch, current: SelfAccess.currentOf(configuration), ...options });
  }
  // `udpSwitch`: whether this container runs the UDP relay at all, and the value .env fixes for media over it (config.mjs).
  constructor({ paths, fixed, read, current, exit, load = null, envAddressesIgnored = false, envSeed = null, savedEnvSeed = null, unreadable = null, udpSwitch = { relay: false, env: null }, gateway = defaultGateway, probeRequest = probeAddress, publicRequest = publicAccessRequest, lookup = lookupPublicIp, log = line => console.log(JSON.stringify(line)), now = Date.now,
    sleep = ms => new Promise(resolve => { const timer = setTimeout(resolve, ms); timer.unref?.(); }),
    testTtlMs = 300000, maxWaitMs = 300000, endWaitMs = 15000, pollMs = 1000, confirmWindowMs = outsideConfirmationMs, verifyAttempts = 12, verifyDelayMs = 1000, maxStarts = 3, followIntervalMs = 600000, followFirstDelayMs = 30000, followRetryMs = 3000, reconcileIntervalMs = 15000 }) {
    Object.assign(this, { paths, fixed, read, current, load, envAddressesIgnored, envSeed, savedEnvSeed, unreadable, probeRequest, publicRequest, lookup, log, now, sleep, verifyAttempts, verifyDelayMs, maxStarts, followIntervalMs, followFirstDelayMs, followRetryMs, reconcileIntervalMs });
    // Once this process has asked to be started again, its rounds are over.
    this.ending = false; this.exit = () => { this.ending = true; exit(); };
    this.testTtlMs = Math.min(testTtlMs, 300000); this.maxWaitMs = Math.min(maxWaitMs, 300000); this.endWaitMs = Math.min(endWaitMs, 60000); this.pollMs = Math.max(1, pollMs);
    this.confirmWindowMs = Math.min(confirmWindowMs, outsideConfirmationMs); this.records = []; this.written = Promise.resolve(); this.udpSwitch = udpSwitch;
    this.tests = new Map(); this.busy = false; this.testing = false; this.task = Promise.resolve(); this.state = { state: 'idle' };
    this.hooks = null; this.pendingVerify = false; this.cancelRequested = false; this.committed = false; this.wake = null;
    this.round = null; this.followError = null; this.timers = []; this.followDue = false; this.retry = null; this.probes = new Map();
    // Offered to the console only on the network compose makes (it names the published port).
    this.gateway = fixed.published ? gateway() : null;
  }
  // The server's own activity report, its "end the connections", its admission
  // fence and its activity log (http.mjs).
  attach(hooks) { this.hooks = hooks; for (const row of this.records.splice(0)) this.record(...row); }
  // The saved switch reaches the running relay, unless .env decides.
  applyUdpMedia() { if (this.udpSwitch.env === null) { try { this.hooks?.udpMedia?.(this.current.udpMedia === true); } catch { /* The next start takes it from the policy. */ } } }
  // One row of the activity log. What happened before the server could write
  // (the result a start found in the journal) is kept until it can.
  record(event, actor, detail) {
    if (!this.hooks) { if (this.records.length < 16) this.records.push([event, actor, detail]); return; }
    try { this.hooks.record?.(event, actor ?? 'server', detail); } catch { /* The log cannot change what was done. */ }
  }
  close() { for (const timer of [...this.timers, this.retry]) clearTimeout(timer); this.timers = []; this.retry = null; }
  // Set once a restarted process still has to reach its new addresses:
  // admission stays closed for this operation until the result is known.
  get startupOperationId() { return this.pendingVerify ? this.state.operationId : null; }
  // One write of the journal at a time, in the order the state changed: a
  // proof that arrives while its window is being closed must not leave an
  // earlier state on the disk.
  set(values) {
    this.state = { ...this.state, ...values };
    const text = JSON.stringify(this.state), write = () => writeText(this.paths.journal, text);
    return this.written = this.written.then(write, write);
  }
  finish(values) { return this.set({ idleReasons: [], blockers: [], waitUntil: null, ...values, previous: null, previousConfig: null, completedAt: new Date(this.now()).toISOString() }); }

  // What the journal says of the start that is running now.
  async initialize() {
    let journal;
    try { journal = readSelfJournal(this.paths, this.read); }
    catch { this.state = { state: 'recovery_required', errorCode: 'server_access_storage_invalid' }; return; }
    if (!journal) return;
    this.state = journal;
    const applied = this.current.revision === journal.revision, previous = this.current.revision === journal.previousRevision;
    try {
      // Nothing had been written yet, also while a save waited for its proof from outside.
      if (journal.state === 'waiting_idle' || journal.state === 'awaiting_confirmation') {
        if (previous) await this.finish({ state: 'failed', errorCode: 'server_access_interrupted', unchangedVerified: true, rollbackVerified: false });
        else this.state = { state: 'idle' };
      } else if (journal.state === 'applying' || journal.state === 'health_check') {
        if (applied) {
          // A process that keeps ending before it can check goes back too.
          if ((journal.starts ?? 0) >= this.maxStarts) await this.rollBack('server_access_start_failed');
          else { await this.set({ state: 'health_check', starts: (journal.starts ?? 0) + 1 }); this.pendingVerify = true; this.busy = true; }
        } else if (previous) await this.finish({ state: 'failed', errorCode: 'server_access_interrupted', unchangedVerified: true, rollbackVerified: false });
        else await this.set({ state: 'recovery_required', errorCode: 'server_access_recovery_required' });
      } else if (journal.state === 'rolling_back') {
        if (previous) {
          await this.finish({ state: 'rolled_back', rollbackVerified: true, unchangedVerified: false, errorCode: journal.errorCode ?? 'server_access_health_failed' });
          this.record('server_access_rolled_back', 'server', journal.operationId + ':' + (journal.failedOrigin ?? ''));
        }
        else if (applied) await this.rollBack(journal.errorCode ?? 'server_access_health_failed');
        else await this.set({ state: 'recovery_required', errorCode: 'server_access_recovery_required' });
      // A result whose policy is no longer the one in use says nothing now.
      } else if ((journal.state === 'succeeded' && !applied) || (['rolled_back', 'failed'].includes(journal.state) && !previous)) this.state = { state: 'idle' };
    } catch { this.state = { ...this.state, state: 'recovery_required', errorCode: 'server_access_recovery_required' }; this.pendingVerify = false; this.busy = false; }
  }
  // Puts the previous policy back and asks for the restart that loads it.
  async rollBack(errorCode, failedOrigin = this.state.failedOrigin ?? null) {
    this.pendingVerify = false;
    await this.set({ state: 'rolling_back', errorCode, failedOrigin });
    await putSelfPolicy(this.paths, this.state.previous ?? null);
    this.exit();
  }
  // After the listeners are up: every main address must answer as this
  // installation with this policy (the check the updater makes after its
  // restart). Extra addresses are not asked: a router may not loop them back.
  // Nor is a main address the administrator proved from outside before the
  // save: this server cannot reach it, and the same listener answers it now.
  async resume() {
    if (!this.pendingVerify) return;
    const { config, healthRevision: revision, installationId } = this.current, proven = this.state.outside?.confirmed ?? [];
    try {
      // `unreached`: the address that did not answer as this installation, for the page to name.
      let passed = false, unreached = null;
      for (let attempt = 0; attempt < this.verifyAttempts && !passed; attempt++) {
        if (attempt) await this.sleep(this.verifyDelayMs);
        passed = true;
        for (const origin of probedServerOrigins(config).filter(item => !proven.includes(item))) if (!validAccessHealth(await this.publicRequest(config, origin), { config, revision, installationId, origin })) { passed = false; unreached = origin; break; }
      }
      if (passed) { this.pendingVerify = false; await this.finish({ state: 'succeeded', errorCode: null, rollbackVerified: false, unchangedVerified: false }); }
      else await this.rollBack('server_access_health_failed', unreached);
    } catch { this.pendingVerify = false; this.state = { ...this.state, state: 'recovery_required', errorCode: 'server_access_recovery_required' }; }
    finally { this.busy = false; }
  }

  dockerPorts() {
    return this.fixed.published ? ['https', 'http'].map(scheme => ({ hostPort: this.fixed.published.port, containerPort: this.fixed.listenPort, protocol: 'tcp', scheme })) : [];
  }
  // `context.managementOrigin`: the address the asking console is open at.
  status(context = null) {
    const state = this.state, unverified = state.state === 'recovery_required', { config, publicIp } = this.current;
    // While a new policy is being checked the previous one is still "applied".
    const checking = ['health_check', 'rolling_back'].includes(state.state);
    const result = { protocol: serverAccessProtocol, managed: true, applySupported: !unverified, deploymentKind: 'docker', selfManaged: true,
      revision: checking ? state.previousRevision : this.current.revision, config: checking ? shown(state.previousConfig, state.previousPublicIp, state.previousUdpMedia === true) : shown(config, publicIp, this.current.udpMedia),
      pendingConfig: selfAccessActiveStates.has(state.state) || unverified ? shown(state.config, state.publicIp, state.udpMedia === true) : null,
      // Media over UDP: what is saved, whether this container runs the relay at all, and the value .env fixes (null: the saved one decides).
      udpSwitch: { saved: this.current.udpMedia === true, relay: this.udpSwitch.relay === true, env: this.udpSwitch.env ?? null },
      state: state.state === 'rolling_back' ? 'health_check' : state.state, legacy: !this.current.saved,
      scope: unverified ? 'unverified' : state.state === 'succeeded' ? 'applied' : 'preflight', testing: this.testing,
      dockerPorts: this.dockerPorts(), plannedDockerPorts: [], rollbackVerified: state.rollbackVerified === true, unchangedVerified: state.unchangedVerified === true,
      requiresProxyConfiguration: false, admissionMarkerCleared: true, envAddressesIgnored: this.envAddressesIgnored, gateway: this.gateway,
      // The reason the saved policy is not in use (config.mjs): the server runs without one until an administrator saves again.
      ...(this.unreadable ? { policyUnreadable: this.unreadable } : {}),
      fixed: { listenHost: this.fixed.listenHost, listenPort: this.fixed.listenPort, httpsMode: this.fixed.httpsMode, certFile: this.fixed.certFile, keyFile: this.fixed.keyFile, idlePorts: this.fixed.idlePorts,
        publishedPort: this.fixed.published?.port ?? null, publishedHost: this.fixed.published?.host ?? null, serverAddress: this.fixed.serverAddress },
      // `offered`: this container can follow at all (its port is published
      // beyond loopback). `origin`: the address registered from the lookup.
      publicIp: { offered: this.fixed.httpsMode === 'proxy' && this.fixed.published !== null && !loopbackPublished(this.fixed), follow: publicIp.follow, origin: followedOrigin(config, publicIp), ...(this.followError ? { errorCode: this.followError } : {}) } };
    if (unverified) { result.currentObserved = true; result.currentRevision = this.current.revision; }
    for (const key of ['operationId', 'errorCode', 'failedOrigin', 'checks', 'idleReasons', 'blockers', 'waitUntil', 'completedAt']) if (state[key] !== undefined && state[key] !== null) result[key] = state[key];
    // A save that waits for its proof from outside: which addresses, how long
    // still, and the code to bring there. The code is given where the save can
    // be watched, never at an address still under proof: it has to be brought.
    if (state.state === 'awaiting_confirmation' && state.outside) {
      const { origins, confirmed, code, until } = state.outside, here = context?.managementOrigin ?? null;
      result.outside = { origins: origins.map(origin => ({ origin, confirmed: confirmed.includes(origin) })), remainingMs: Math.max(0, Date.parse(until) - this.now()), windowMs: this.confirmWindowMs,
        ...(typeof here === 'string' && (!origins.includes(here) || confirmed.includes(here)) ? { code } : {}) };
    }
    return result;
  }
  context(value, { save = false } = {}) {
    if (!record(value) || Object.keys(value).some(key => !['managementOrigin', 'fence'].includes(key))) fail('context_invalid');
    let managementOrigin;
    try { managementOrigin = normalizeBrokerOrigin(value.managementOrigin, { allowInsecureHttp: true }); } catch { fail('management_origin_invalid'); }
    if (this.current.config && !allowedServerOrigins(this.current.config).includes(managementOrigin)) fail('management_origin_changed');
    if (save && (!exact(value.fence, ['token', 'operationId']) || !hex.test(value.fence.token ?? '') || !uuid.test(value.fence.operationId ?? ''))) fail('fence_invalid');
    return { managementOrigin, ...(save ? { fence: value.fence } : {}) };
  }
  // What the console sent, as this container would run it.
  // The switch for media over UDP is the console's to change only where it decides anything.
  candidate(value) {
    const { config, follow, udpMedia } = splitFollow(value), open = this.udpSwitch.relay === true && this.udpSwitch.env === null;
    return selfAccessCandidate(config, follow, this, open ? udpMedia === true : this.current.udpMedia === true);
  }
  enter() {
    if (this.busy) fail('busy');
    if (this.state.state === 'recovery_required') fail('recovery_required');
    this.busy = true;
  }
  async test(input, context) {
    try {
      if (!exact(input, ['config', 'baseRevision'])) fail('command_invalid');
      this.enter();
    } catch (error) { throw httpError(error); }
    this.testing = true;
    try {
      const trusted = this.context(context);
      if (input.baseRevision !== this.current.revision) fail('revision_changed');
      const { config, revision: configHash } = this.candidate(input.config), expires = this.now() + this.testTtlMs;
      // The addresses in use stay as they are (only a switch changes): nothing is asked of them, nothing restarts.
      const addressesUnchanged = !!this.current.config && serverAccessHash(config) === this.current.healthRevision;
      const checks = [{ name: 'configuration', status: 'passed' }, { name: 'managed_installation', status: 'passed' }, ...(addressesUnchanged ? [] : await this.reach(config))];
      // A main address that does not lead here: no test to save with (the save
      // would end every connection and then go back), unless every such address
      // only looks the way one that works from outside can. That test can be
      // saved with the administrator's own proof, and in no other way. An extra
      // address this server did not reach is 'unreached', not failed (reach()).
      const failed = checks.filter(check => check.status === 'failed'), outside = failed.every(check => outsideReasons.has(check.code)) ? failed.map(check => check.origin) : null;
      if (failed.length && !outside) return { baseRevision: this.current.revision, configHash, scope: 'preflight', checks, plannedDockerPorts: this.dockerPorts(), requiresRestart: true };
      for (const [id, test] of this.tests) if (test.expires <= this.now()) this.tests.delete(id);
      while (this.tests.size >= 8) this.tests.delete(this.tests.keys().next().value);
      const testId = createHash('sha256').update(JSON.stringify({ configHash, baseRevision: this.current.revision, expires, nonce: randomBytes(32).toString('hex') })).digest('hex');
      this.tests.set(testId, { configHash, baseRevision: this.current.revision, expires, managementOrigin: trusted.managementOrigin, checks, outside: outside ?? [] });
      return { testId, baseRevision: this.current.revision, configHash, expiresAt: new Date(expires).toISOString(), scope: 'preflight', checks, plannedDockerPorts: this.dockerPorts(), requiresRestart: true,
        ...(addressesUnchanged ? { addressesUnchanged: true } : {}), ...(outside?.length ? { confirmation: 'outside', confirmationWindowMs: this.confirmWindowMs } : {}) };
    } catch (error) { throw httpError(error); }
    finally { this.testing = false; this.busy = false; }
  }
  // One check per address of `config`, the main ones first: does it lead to
  // this server, and would the request be let in under that policy? `peer` is
  // where the request arrived from (the reverse proxy, when one is in between).
  // A failed check carries the reason as its code: one of probeAddress's, or
  //   other_server     something else answers there
  //   proxy_untrusted  it arrived here from `peer`, which is not a trusted proxy
  //   proxy_headers    a trusted proxy that did not pass the scheme and the name on
  //   not_admitted     it arrived here but is not this address
  // An extra address (`extra: true`) is typically a router forward that this
  // server may not reach from inside (no NAT loopback): one that only looks
  // like that (outsideReasons) is 'unreached', said and saved without a proof,
  // as the check after the restart does not ask extra addresses either. A
  // fault on it (a certificate, another server, an untrusted proxy, not HTTPS)
  // fails like one on a main address.
  async reach(config) {
    const main = probedServerOrigins(config), extras = allowedServerOrigins(config).filter(origin => !main.includes(origin));
    return Promise.all([...main.map(origin => [origin, false]), ...extras.map(origin => [origin, true])].map(async ([origin, extra]) => {
      const found = await this.probe(config, origin);
      return { name: 'address', origin, ...(extra ? { extra: true } : {}), ...found, ...(extra && found.status === 'failed' && outsideReasons.has(found.code) ? { status: 'unreached' } : {}) };
    }));
  }
  async probe(config, origin) {
    const nonce = randomBytes(32).toString('hex');
    this.probes.set(nonce, { config, origin });
    try {
      const result = await this.probeRequest(origin, nonce);
      if (result?.error) return { status: 'failed', code: 'server_access_address_' + (['name_unknown', 'unchecked', 'closed', 'not_https', 'certificate'].includes(result.error) ? result.error : 'no_answer') };
      const answer = result?.answer;
      if (!record(answer) || answer.protocol !== accessProbeProtocol || answer.nonce !== nonce) return { status: 'failed', code: 'server_access_address_other_server' };
      const peer = typeof answer.peer === 'string' && isIP(answer.peer) ? { peer: answer.peer } : {};
      return answer.admitted === true ? { status: 'passed', code: 'server_access_address_reached' }
        : { ...peer, status: 'failed', code: 'server_access_address_' + (['proxy_untrusted', 'proxy_headers'].includes(answer.reason) ? answer.reason : 'not_admitted') };
    } catch { return { status: 'failed', code: 'server_access_address_no_answer' }; }
    finally { this.probes.delete(nonce); }
  }
  // http.mjs, for GET /api/v1/access-probe/<value> on any address: the answer
  // to this server's own question, or null for anything else. It says whether
  // this very request would be let in at the asked address by the policy under
  // test (the scheme, a trusted proxy's forwarded headers, the listener).
  answerProbe(nonce, req) {
    const probe = this.probes.get(nonce);
    if (!probe) return null;
    let admitted = false;
    try { const access = resolveRequestAccess(req, probe.config); admitted = access.origin === probe.origin && access.bootstrapOnly !== true; } catch { admitted = false; }
    const peer = String(req.socket?.remoteAddress ?? '').replace(/^::ffff:(?=\d{1,3}(?:\.\d{1,3}){3}$)/i, '');
    // An HTTPS address behind a reverse proxy is let in only from a trusted
    // proxy that says the scheme and the name it was asked for.
    const viaProxy = !admitted && probe.origin.startsWith('https://') && probe.config.https.mode === 'proxy' && req.socket?.encrypted !== true;
    const reason = !viaProxy ? null : !probe.config.trustedProxies.includes(peer) ? 'proxy_untrusted' : 'proxy_headers';
    return { protocol: accessProbeProtocol, nonce, admitted, ...(reason ? { reason } : {}), ...(isIP(peer) ? { peer } : {}) };
  }
  // The journal of a change, written before anything else happens.
  begin({ operationId, config, publicIp, udpMedia = this.current.udpMedia === true, revision, checks = [], kind = 'save', state = 'waiting_idle', outside = null }) {
    this.cancelRequested = false; this.committed = false;
    const previous = this.read(this.paths.policy, 32768);
    this.state = {};
    return this.set({ schema: 1, operationId, kind, state, revision, config, publicIp, udpMedia, previousRevision: this.current.revision, previousConfig: this.current.config, previousPublicIp: this.current.publicIp, previousUdpMedia: this.current.udpMedia === true, previous,
      checks, errorCode: null, rollbackVerified: false, unchangedVerified: false, idleReasons: [], blockers: [], waitUntil: null, starts: 0, ...(outside ? { outside } : {}) });
  }
  async save(input, context) {
    let next, trusted;
    try {
      if (!exact(input, ['config', 'testId', 'baseRevision', 'confirm']) || input.confirm !== true || !hex.test(input.testId ?? '')) fail('confirmation_required');
      if (!this.hooks) fail('unavailable');
      this.enter();
    } catch (error) { throw httpError(error); }
    try {
      trusted = this.context(context, { save: true });
      if (input.baseRevision !== this.current.revision) fail('revision_changed');
      next = this.candidate(input.config);
      const test = this.tests.get(input.testId);
      if (!test || test.expires <= this.now() || test.baseRevision !== this.current.revision || test.configHash !== next.revision || test.managementOrigin !== trusted.managementOrigin) fail('test_required');
      // A test with an address this server did not reach is saved only with the proof from outside (saveOutside).
      if (test.outside.length) fail('test_required');
      if (next.revision === this.current.revision) fail('change_required');
      if (trusted.fence.operationId === this.state.operationId) fail('operation_reused');
      this.tests.clear();
      await this.begin({ operationId: trusted.fence.operationId, ...next, checks: test.checks });
    } catch (error) {
      this.busy = false;
      const refusal = httpError(error);
      // Any other end of an accepted fence is said as a result, so the fence
      // AccessRuntime keeps for a lost answer opens on its next status read.
      if (![400, 409].includes(refusal.status) && uuid.test(context?.fence?.operationId ?? '')) this.state = { state: 'failed', operationId: context.fence.operationId, errorCode: refusal.code, unchangedVerified: true, rollbackVerified: false };
      throw refusal;
    }
    this.task = this.perform(next).finally(() => { this.busy = false; });
    return { accepted: true, operationId: trusted.fence.operationId, state: 'waiting_idle' };
  }
  // A save with the administrator's own proof of the addresses this server
  // could not reach (see the top of this file). Accepted here, it only opens
  // the window; `admitted(config)` is the address this very request would be
  // let in at under `config` (http.mjs), and `actor` who asked.
  async saveOutside(input, context, { actor = null, admitted = null } = {}) {
    let next, operationId, awaited;
    try {
      if (!exact(input, ['config', 'testId', 'baseRevision', 'confirm', 'outside']) || input.confirm !== true || input.outside !== true || !hex.test(input.testId ?? '')) fail('confirmation_required');
      if (!this.hooks?.hold) fail('unavailable');
      this.enter();
    } catch (error) { throw httpError(error); }
    try {
      const trusted = this.context(context);
      if (input.baseRevision !== this.current.revision) fail('revision_changed');
      next = this.candidate(input.config);
      const test = this.tests.get(input.testId);
      if (!test || !test.outside.length || test.expires <= this.now() || test.baseRevision !== this.current.revision || test.configHash !== next.revision || test.managementOrigin !== trusted.managementOrigin) fail('test_required');
      if (next.revision === this.current.revision) fail('change_required');
      this.tests.clear();
      operationId = randomUUID(); awaited = test.outside;
      // The request that saves it is the proof for the address it came in on.
      const here = admittedOrigin(admitted, next.config), confirmed = awaited.filter(origin => origin === here && origin === trusted.managementOrigin);
      await this.begin({ operationId, ...next, checks: test.checks, state: 'awaiting_confirmation',
        outside: { origins: awaited, confirmed, code: String(randomInt(0, 1000000)).padStart(6, '0'), until: new Date(this.now() + this.confirmWindowMs).toISOString(), actor } });
    } catch (error) { this.busy = false; throw httpError(error); }
    this.record('server_access_outside_started', actor, operationId + ':' + awaited.join(' '));
    this.task = this.performOutside(next).finally(() => { this.busy = false; });
    return { accepted: true, operationId, state: 'awaiting_confirmation' };
  }
  // True while that save still waits for its proof (AccessRuntime: it holds no fence yet, and can be withdrawn).
  awaiting(operationId) { return this.busy && this.state.state === 'awaiting_confirmation' && this.state.operationId === operationId; }
  // The proof: a signed-in administrator's request that this server lets in at
  // an address under proof, by the policy being saved, with the code shown
  // where the save was made. The same request at any other address, or
  // without that code, proves nothing about this one.
  async confirmOutside(input, context, { actor = null, admitted = null } = {}) {
    try {
      if (!exact(input, ['operationId', 'code']) || !uuid.test(input.operationId ?? '') || typeof input.code !== 'string' || !/^\d{6}$/.test(input.code)) fail('command_invalid');
      const state = this.state, outside = state.outside;
      if (state.state !== 'awaiting_confirmation' || state.operationId !== input.operationId || !outside || this.now() >= Date.parse(outside.until)) fail('confirmation_unavailable');
      const origin = admittedOrigin(admitted, state.config);
      if (!origin || origin !== context?.managementOrigin || !outside.origins.includes(origin)) fail('confirmation_origin');
      if (!timingSafeEqual(Buffer.from(input.code), Buffer.from(outside.code))) fail('confirmation_code');
      if (!outside.confirmed.includes(origin)) {
        await this.set({ outside: { ...outside, confirmed: [...outside.confirmed, origin] } });
        this.record('server_access_outside_confirmed', actor, state.operationId + ':' + origin);
      }
      this.wake?.();
      return { accepted: true, operationId: state.operationId, origin, remaining: this.state.outside.origins.filter(item => !this.state.outside.confirmed.includes(item)) };
    } catch (error) { throw httpError(error); }
  }
  // http.mjs, for a request the policy in use does not let in: while a save
  // waits for its proof, and until that save has ended, an address under proof
  // is let in as the policy being saved would let it in. http.mjs serves such
  // a request the console and nothing else.
  provisionalAccess(req) {
    const state = this.state;
    if (!state.outside || !['awaiting_confirmation', 'waiting_idle', 'applying'].includes(state.state)) return null;
    if (state.state === 'awaiting_confirmation' && this.now() >= Date.parse(state.outside.until)) return null;
    try {
      const access = resolveRequestAccess(req, state.config);
      return state.outside.origins.includes(access.origin) && access.bootstrapOnly !== true ? { ...access, provisional: true } : null;
    } catch { return null; }
  }
  // The window: until every address is proven, the save is withdrawn or the
  // time is up. Nothing is ended, written or fenced meanwhile.
  async performOutside(next) {
    const { operationId, outside } = this.state, until = Date.parse(outside.until);
    try {
      while (this.state.outside.confirmed.length < outside.origins.length) {
        if (this.cancelRequested) fail('cancelled');
        if (this.now() >= until) fail('confirmation_timeout');
        await this.pause(Math.max(1, Math.min(this.pollMs, until - this.now())));
      }
      // Proven: from here it is the save any other is, behind the same fence.
      try { this.hooks.hold({ operationId, actor: outside.actor, event: 'server_access_connections_ended', observe: () => this.state.operationId !== operationId || ['succeeded', 'failed', 'rolled_back'].includes(this.state.state) }); }
      catch { fail('busy'); }
      await this.set({ state: 'waiting_idle', outside: { ...this.state.outside, code: null } });
    } catch (error) {
      const errorCode = codeOf(error), unproven = this.state.outside.origins.filter(origin => !this.state.outside.confirmed.includes(origin));
      try {
        await this.finish({ state: 'failed', errorCode, unchangedVerified: true, rollbackVerified: false, outside: { ...this.state.outside, code: null }, ...(errorCode === 'server_access_confirmation_timeout' ? { failedOrigin: unproven[0] ?? null } : {}) });
        if (errorCode === 'server_access_confirmation_timeout') this.record('server_access_outside_expired', outside.actor, operationId + ':' + unproven.join(' '));
      } catch { this.state = { ...this.state, state: 'recovery_required', errorCode: 'server_access_recovery_required' }; }
      return;
    }
    await this.perform(next);
  }
  // Withdraws a save that still waits, for its proof from outside or for the
  // server's own work: nothing has been ended or written yet.
  async cancel(input) {
    try {
      if (!exact(input, ['operationId']) || !uuid.test(input.operationId ?? '')) fail('command_invalid');
      if (!this.busy || !['waiting_idle', 'awaiting_confirmation'].includes(this.state.state) || this.state.operationId !== input.operationId) fail('cancel_unavailable');
      if (this.committed) fail('cancel_too_late');
    } catch (error) { throw httpError(error); }
    this.cancelRequested = true; this.wake?.();
    await this.task;
    return { accepted: true, operationId: input.operationId, state: this.state.state, cancelled: this.state.operationId === input.operationId && this.state.errorCode === 'server_access_cancelled' };
  }
  observe() {
    const report = this.hooks.activity();
    if (report?.protocol !== 'server-access-activity-v1' || report.fence !== true) fail('fence_lost');
    return serverAccessQuiescence(report);
  }
  pause(ms) { return Promise.race([this.sleep(ms), new Promise(resolve => { this.wake = resolve; })]); }
  // Stage 1: a bounded, cancellable wait for the server's own unfinished work
  // (a Host update, a desktop setting, a power task). Connections stay up.
  async settle() {
    const deadline = this.now() + this.maxWaitMs; let displayed = '[]';
    while (true) {
      if (this.cancelRequested) fail('cancelled');
      const { blockers } = this.observe();
      if (!blockers.length) { this.committed = true; if (displayed !== '[]') await this.set({ idleReasons: [], blockers: [], waitUntil: null }); return; }
      const text = JSON.stringify(blockers);
      if (text !== displayed) { displayed = text; await this.set({ idleReasons: blockers.map(row => row.code), blockers, waitUntil: new Date(deadline).toISOString() }); }
      if (this.now() >= deadline) fail('idle_timeout');
      await this.pause(this.pollMs);
    }
  }
  // Stage 2: the server ends every remaining connection and goes on once two
  // reports in a row show none.
  async endConnections() {
    const deadline = this.now() + this.endWaitMs;
    while (true) {
      const first = this.observe();
      if (first.blockers.length) fail('activity_changed');
      if (!first.open) {
        await this.sleep(Math.min(250, this.pollMs));
        const second = this.observe();
        if (second.blockers.length) fail('activity_changed');
        if (!second.open) return;
      }
      if (this.now() >= deadline) fail('connections_busy');
      const ended = this.hooks.endConnections();
      if (ended?.protocol !== 'server-access-connections-v1' || ended.fence !== true) fail('fence_lost');
      await this.sleep(Math.min(250, this.pollMs));
    }
  }
  async perform({ config, publicIp, udpMedia = this.current.udpMedia === true, revision }) {
    let written = false;
    try {
      // An administrator's save is made beside the .env lines of now; the
      // server's own change of the public address keeps what was remembered.
      const saving = this.state.kind !== 'public-ip', envSeed = saving ? this.envSeed : this.savedEnvSeed;
      const text = selfPolicyText(config, publicIp, this.now, envSeed, udpMedia);
      // Only a switch changes (the public IP followed or not, media over UDP or
      // not): the addresses in use stay, so nobody is disconnected and nothing restarts.
      if (this.current.config && serverAccessHash(config) === this.current.healthRevision) {
        await ensureSelfIdentity(this.paths, this.read);
        written = true;
        await putSelfPolicy(this.paths, text);
        await this.finish({ state: 'succeeded', errorCode: null });
        this.current = { ...this.current, publicIp, udpMedia, revision, saved: true };
        this.applyUdpMedia();
        if (saving) { this.savedEnvSeed = envSeed; this.envAddressesIgnored = false; }
        // Turned on by a save: the first round is due now. The save's own fence
        // is given back first (the runtime reads this result), or that round
        // could not take it.
        if (publicIp.follow && saving) { this.followDue = true; try { await this.hooks.settle?.(); } catch { /* The round waits for the fence instead. */ } this.later(0); }
        return;
      }
      await this.settle();
      await this.endConnections();
      await ensureSelfIdentity(this.paths, this.read);
      // The journal names the way back before the policy changes.
      await this.set({ state: 'applying', idleReasons: [], blockers: [], waitUntil: null });
      const last = this.observe();
      if (last.blockers.length || last.open) fail('activity_changed');
      written = true;
      await putSelfPolicy(this.paths, text);
      this.exit();
    } catch (error) {
      const errorCode = codeOf(error);
      try {
        if (written) await putSelfPolicy(this.paths, this.state.previous ?? null);
        // A timeout keeps what it waited for; every other end clears it.
        await this.finish({ state: 'failed', errorCode, unchangedVerified: true, rollbackVerified: false, ...(errorCode === 'server_access_idle_timeout' ? { idleReasons: this.state.idleReasons, blockers: this.state.blockers } : {}) });
      } catch { this.state = { ...this.state, state: 'recovery_required', errorCode: 'server_access_recovery_required' }; }
    }
  }

  // ---- the volume under a running server -------------------------------------
  // index.mjs starts the rounds once the server listens: the look at the
  // volume every few seconds, and the public IP every ten minutes (a round
  // with following off asks nothing).
  startRounds() {
    this.later(this.followFirstDelayMs);
    for (const [run, ms] of [[() => this.followPublicIp(), this.followIntervalMs], [() => this.look(), this.reconcileIntervalMs]]) {
      const timer = setInterval(() => { void run().catch(() => {}); }, ms); timer.unref?.();
      this.timers.push(timer);
    }
  }
  // The look at the volume; a round that is due (following turned on from the
  // command line, or one that could not start) runs right after it.
  async look() {
    await this.reconcile();
    if (this.followDue) await this.followPublicIp();
  }
  later(ms) { const timer = setTimeout(() => { void this.followPublicIp(); }, ms); timer.unref?.(); this.timers.push(timer); }
  // A round that could not start is due, and is tried again in a few seconds
  // rather than at the next ten-minute round.
  soon() {
    this.followDue = true;
    if (this.retry) return;
    this.retry = setTimeout(() => { this.retry = null; void this.followPublicIp(); }, this.followRetryMs); this.retry.unref?.();
  }
  // What the volume holds now against what this process runs. access-cli.mjs
  // may have written a policy (also the first one, with its installation ID),
  // replaced it or removed it. The same addresses: the rest is taken over and
  // nobody is disconnected. Other addresses: the server cannot serve them
  // without starting again, so it ends the connections and does (the command
  // that wrote them may start it again sooner). Nothing is done while a save
  // of this server is in flight, while its own work would be cut in half, or
  // for a policy that does not load: that one keeps the health check failing.
  async reconcile() {
    if (!this.load || this.ending || this.busy || !this.hooks?.hold || selfAccessActiveStates.has(this.state.state) || this.state.state === 'recovery_required') return;
    let loaded;
    try { loaded = this.load(); } catch { return; }
    const self = loaded?.selfAccess;
    // A server that runs a policy is not started again into one it could not read.
    if (!self || (self.unreadable && !this.unreadable)) return;
    const next = SelfAccess.currentOf(loaded);
    if (next.healthRevision === this.current.healthRevision) {
      if (next.publicIp.follow && !this.current.publicIp.follow) this.followDue = true;
      // Also "nothing to serve" on both sides: an unreadable policy that
      // `access-cli.mjs reset` removed is no longer the reason for it.
      this.current = next; this.savedEnvSeed = self.savedEnvSeed; this.envAddressesIgnored = self.envAddressesIgnored; this.unreadable = self.unreadable ?? null;
      this.applyUdpMedia();
      return;
    }
    try {
      if (this.observe().blockers.length) return;
      // The fence of a save: nothing new begins, and it stays until this process has ended.
      this.hooks.hold({ operationId: randomUUID(), event: 'server_access_connections_ended', observe: () => false });
    } catch { return; }
    this.log({ service: 'multitap-broker', event: 'server_access_policy_changed', message: 'The saved server access settings changed while the server was running. It starts again to use them.' });
    try { this.hooks.endConnections(); } catch { /* The restart ends them in any case. */ }
    this.exit();
  }

  // ---- following the public IP ----------------------------------------------
  // One round at a time: ask for the public IP and, when the registered address
  // is another one, replace it through the same journaled change a save makes.
  followPublicIp() { return this.round ??= this.followRound().finally(() => { this.round = null; }); }
  async followRound() {
    // The switch as the volume has it now (`access-cli.mjs public-ip on|off`).
    await this.reconcile();
    if (this.ending || !this.hooks?.hold || this.state.state === 'recovery_required') return;
    const { config, publicIp } = this.current;
    if (!publicIp.follow || !publicIpOffered(config, this.fixed)) { this.followDue = false; return; }
    // Not now: a change is in flight, or the fence of one has not been given
    // back yet. Nothing is asked; the round stays due.
    if (this.busy || selfAccessActiveStates.has(this.state.state) || this.hooks.blocked?.()) { this.soon(); return; }
    this.followDue = false;
    let lease = null;
    try {
      const ip = publicIpv4(await this.lookup());
      // No usable answer: the registered address stays; the next round asks again.
      if (!ip || !this.current.publicIp.follow) return;
      if (this.busy) { this.soon(); return; }
      const origin = 'http://' + ip + ':' + this.fixed.published.port, list = endpointOrigins(config.http), known = followedOrigin(config, publicIp);
      if (origin === known) { this.followError = null; return; }
      // A registration that was put back is not tried again for the same address.
      if (this.state.kind === 'public-ip' && this.state.state === 'rolled_back' && this.state.publicIp?.origin === origin) return;
      const others = list.filter(item => item !== known && item !== origin);
      if (!others.length || config.primaryOrigin === origin) { this.followError = 'server_access_public_ip_conflict'; return; }
      if (others.length > maxExtraOrigins) { this.followError = 'server_access_http_addresses_full'; return; }
      this.followError = null;
      const next = { config: selfAccessPolicy({ primaryOrigin: config.primaryOrigin, https: enabledOrigins(config.https), http: [...others, origin], trustedProxies: config.trustedProxies }, this.fixed), publicIp: { follow: true, origin }, udpMedia: this.current.udpMedia === true };
      next.revision = selfPolicyRevision(next.config, next.publicIp, next.udpMedia);
      const operationId = randomUUID();
      // The fence a save holds; it opens here once this process knows the end,
      // and in the restarted process by its own status.
      try { lease = this.hooks.hold({ operationId, event: 'server_access_connections_ended', observe: () => this.state.operationId !== operationId || ['succeeded', 'failed', 'rolled_back'].includes(this.state.state) }); }
      catch { this.soon(); return; } // Somebody took the fence meanwhile: the registration is still due.
      this.busy = true;
      try {
        try { await this.begin({ operationId, ...next, kind: 'public-ip' }); } catch (error) { this.state = { state: 'idle' }; throw error; }
        this.log({ service: 'multitap-broker', event: 'public_ip_address_changed', from: known, to: origin });
        await this.perform(next);
      } finally { this.busy = false; }
    } catch { if (lease) this.hooks.release?.(lease); this.soon(); }
  }
}
