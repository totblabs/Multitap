// Server access settings of a Docker Compose install that has no updater.
// The broker keeps the policy in its own data volume and applies a saved
// change itself: it writes the new policy, asks to be started again (the
// entrypoint restarts it on selfAccessRestartCode), and the new process
// reaches each main address before it keeps the change. An address that does
// not answer as this installation puts the previous policy back.
//
// Files, all in <data>/access and owned by the broker's own account:
//   policy.json            the saved policy: the shape of server-access.json,
//                          plus `publicIp` while the public IP is followed
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
import { randomBytes, randomUUID, createHash } from 'node:crypto';
import { HttpError } from './errors.mjs';
import { deploymentTransport, normalizeBrokerOrigin } from './transport-policy.mjs';
import { normalizeServerAccess, serverAccessHash, serverAccessProtocol, serverAccessUnconfiguredRevision, decodeServerAccessState, allowedServerOrigins, probedServerOrigins, endpointOrigins, maxExtraOrigins } from '../update/server-access-policy.mjs';
import { serverAccessQuiescence } from '../update/server-access-engine.mjs';
import { publicAccessRequest, validAccessHealth } from '../update/server-access-health.mjs';

// The broker exits with this code to be started again in the same container.
export const selfAccessRestartCode = 75;
export const selfAccessActiveStates = new Set(['waiting_idle', 'applying', 'health_check', 'rolling_back']);
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
const statusOf = code => /_(busy|revision_changed|test_required|fence_lost|management_origin_changed|operation_reused|cancel_unavailable|cancel_too_late)$/.test(code) ? 409
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
export function selfPolicyRevision(config, publicIp = notFollowing) {
  const base = config ? serverAccessHash(config) : serverAccessUnconfiguredRevision;
  return publicIp.follow ? createHash('sha256').update(base + ':public-ip:' + (publicIp.origin ?? '')).digest('hex') : base;
}
// The .env address lines as one value, null when there are none. A policy
// keeps the value an administrator's save was made beside: lines that are
// still those were never meant to change the saved policy, while lines edited
// afterwards were (config.mjs warns that they are ignored).
export function selfEnvSeed(env) {
  const lines = [env.MULTITAP_PUBLIC_ORIGIN ?? '', env.MULTITAP_HTTP_ORIGIN ?? ''];
  return lines.some(Boolean) ? createHash('sha256').update(JSON.stringify(lines)).digest('hex') : null;
}
export function selfPolicyText(config, publicIp = notFollowing, now = Date.now, envSeed = null) {
  const normalized = normalizeServerAccess(config);
  return JSON.stringify({ schema: 1, protocol: serverAccessProtocol, revision: serverAccessHash(normalized), config: normalized, updatedAt: new Date(now()).toISOString(),
    ...(publicIp.follow ? { publicIp: { follow: true, origin: followedOrigin(normalized, publicIp) } } : {}), ...(envSeed ? { envSeed } : {}) });
}
// A policy file as this container runs it.
export function decodeSelfPolicy(text, fixed) {
  let state, follow, seed;
  try {
    const { publicIp, envSeed = null, ...rest } = JSON.parse(text);
    if (envSeed !== null && !hex.test(envSeed)) throw Error();
    state = decodeServerAccessState(rest); follow = publicIp; seed = envSeed;
  } catch { fail('state_invalid'); }
  const config = conformSelfAccess(state.config, fixed);
  return { config, updatedAt: state.updatedAt, publicIp: decodeFollow(follow, config), envSeed: seed };
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
// is followed, and the revision of both. `current` is what runs now; its
// registered public address, when the new list still has it, stays the last
// extra HTTP address.
export function selfAccessCandidate(value, follow, { fixed, current }) {
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
  return { config, publicIp, revision: selfPolicyRevision(config, publicIp) };
}

// The policy as the console edits it: the HTTP section says whether the
// public IP is followed.
const shown = (config, publicIp) => config && publicIp?.follow ? { ...config, http: { ...config.http, followPublicIp: true } } : config ?? null;
function splitFollow(value) {
  if (!record(value) || !record(value.http) || !Object.hasOwn(value.http, 'followPublicIp')) return { config: value, follow: false };
  const { followPublicIp, ...http } = value.http;
  if (typeof followPublicIp !== 'boolean') fail('invalid');
  return { config: { ...value, http }, follow: followPublicIp };
}

// The same four calls the updater's control socket answers
// (server-access-client.mjs), made in this process. AccessRuntime holds the
// admission fence around them exactly as it does for the updater.
export class SelfAccess {
  // `configuration` is config.mjs's result for a self-managed install.
  // What a process runs, from what config.mjs loaded.
  static currentOf(configuration) {
    const { saved, revision, publicIp } = configuration.selfAccess;
    return { config: configuration.accessConfig, healthRevision: configuration.accessRevision, revision, saved, publicIp, installationId: configuration.accessInstallationId };
  }
  // `load` loads the configuration again (reconcile).
  static from(configuration, options) {
    const { paths, fixed, read, envAddressesIgnored, envSeed, savedEnvSeed, unreadable } = configuration.selfAccess;
    return new SelfAccess({ paths, fixed, read, envAddressesIgnored, envSeed, savedEnvSeed, unreadable, current: SelfAccess.currentOf(configuration), ...options });
  }
  constructor({ paths, fixed, read, current, exit, load = null, envAddressesIgnored = false, envSeed = null, savedEnvSeed = null, unreadable = null, gateway = defaultGateway, publicRequest = publicAccessRequest, lookup = lookupPublicIp, log = line => console.log(JSON.stringify(line)), now = Date.now,
    sleep = ms => new Promise(resolve => { const timer = setTimeout(resolve, ms); timer.unref?.(); }),
    testTtlMs = 300000, maxWaitMs = 300000, endWaitMs = 15000, pollMs = 1000, verifyAttempts = 12, verifyDelayMs = 1000, maxStarts = 3, followIntervalMs = 600000, followFirstDelayMs = 30000, followRetryMs = 3000, reconcileIntervalMs = 15000 }) {
    Object.assign(this, { paths, fixed, read, current, load, envAddressesIgnored, envSeed, savedEnvSeed, unreadable, publicRequest, lookup, log, now, sleep, verifyAttempts, verifyDelayMs, maxStarts, followIntervalMs, followFirstDelayMs, followRetryMs, reconcileIntervalMs });
    // Once this process has asked to be started again, its rounds are over.
    this.ending = false; this.exit = () => { this.ending = true; exit(); };
    this.testTtlMs = Math.min(testTtlMs, 300000); this.maxWaitMs = Math.min(maxWaitMs, 300000); this.endWaitMs = Math.min(endWaitMs, 60000); this.pollMs = Math.max(1, pollMs);
    this.tests = new Map(); this.busy = false; this.testing = false; this.task = Promise.resolve(); this.state = { state: 'idle' };
    this.hooks = null; this.pendingVerify = false; this.cancelRequested = false; this.committed = false; this.wake = null;
    this.round = null; this.followError = null; this.timers = []; this.followDue = false; this.retry = null;
    // Offered to the console only on the network compose makes (it names the published port).
    this.gateway = fixed.published ? gateway() : null;
  }
  // The server's own activity report, its "end the connections" and its
  // admission fence (http.mjs).
  attach(hooks) { this.hooks = hooks; }
  close() { for (const timer of [...this.timers, this.retry]) clearTimeout(timer); this.timers = []; this.retry = null; }
  // Set once a restarted process still has to reach its new addresses:
  // admission stays closed for this operation until the result is known.
  get startupOperationId() { return this.pendingVerify ? this.state.operationId : null; }
  async set(values) { this.state = { ...this.state, ...values }; await writeText(this.paths.journal, JSON.stringify(this.state)); }
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
      if (journal.state === 'waiting_idle') {
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
        if (previous) await this.finish({ state: 'rolled_back', rollbackVerified: true, unchangedVerified: false, errorCode: journal.errorCode ?? 'server_access_health_failed' });
        else if (applied) await this.rollBack(journal.errorCode ?? 'server_access_health_failed');
        else await this.set({ state: 'recovery_required', errorCode: 'server_access_recovery_required' });
      // A result whose policy is no longer the one in use says nothing now.
      } else if ((journal.state === 'succeeded' && !applied) || (['rolled_back', 'failed'].includes(journal.state) && !previous)) this.state = { state: 'idle' };
    } catch { this.state = { ...this.state, state: 'recovery_required', errorCode: 'server_access_recovery_required' }; this.pendingVerify = false; this.busy = false; }
  }
  // Puts the previous policy back and asks for the restart that loads it.
  async rollBack(errorCode) {
    this.pendingVerify = false;
    await this.set({ state: 'rolling_back', errorCode });
    await putSelfPolicy(this.paths, this.state.previous ?? null);
    this.exit();
  }
  // After the listeners are up: every main address must answer as this
  // installation with this policy (the check the updater makes after its
  // restart). Extra addresses are not asked: a router may not loop them back.
  async resume() {
    if (!this.pendingVerify) return;
    const { config, healthRevision: revision, installationId } = this.current;
    try {
      let passed = false;
      for (let attempt = 0; attempt < this.verifyAttempts && !passed; attempt++) {
        if (attempt) await this.sleep(this.verifyDelayMs);
        passed = true;
        for (const origin of probedServerOrigins(config)) if (!validAccessHealth(await this.publicRequest(config, origin), { config, revision, installationId, origin })) { passed = false; break; }
      }
      if (passed) { this.pendingVerify = false; await this.finish({ state: 'succeeded', errorCode: null, rollbackVerified: false, unchangedVerified: false }); }
      else await this.rollBack('server_access_health_failed');
    } catch { this.pendingVerify = false; this.state = { ...this.state, state: 'recovery_required', errorCode: 'server_access_recovery_required' }; }
    finally { this.busy = false; }
  }

  dockerPorts() {
    return this.fixed.published ? ['https', 'http'].map(scheme => ({ hostPort: this.fixed.published.port, containerPort: this.fixed.listenPort, protocol: 'tcp', scheme })) : [];
  }
  status() {
    const state = this.state, unverified = state.state === 'recovery_required', { config, publicIp } = this.current;
    // While a new policy is being checked the previous one is still "applied".
    const checking = ['health_check', 'rolling_back'].includes(state.state);
    const result = { protocol: serverAccessProtocol, managed: true, applySupported: !unverified, deploymentKind: 'docker', selfManaged: true,
      revision: checking ? state.previousRevision : this.current.revision, config: checking ? shown(state.previousConfig, state.previousPublicIp) : shown(config, publicIp),
      pendingConfig: selfAccessActiveStates.has(state.state) || unverified ? shown(state.config, state.publicIp) : null,
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
    for (const key of ['operationId', 'errorCode', 'checks', 'idleReasons', 'blockers', 'waitUntil', 'completedAt']) if (state[key] !== undefined && state[key] !== null) result[key] = state[key];
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
  candidate(value) {
    const { config, follow } = splitFollow(value);
    return selfAccessCandidate(config, follow, this);
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
      const { revision: configHash } = this.candidate(input.config), expires = this.now() + this.testTtlMs;
      // The addresses themselves are reached only after the restart.
      const checks = [{ name: 'configuration', status: 'passed' }, { name: 'managed_installation', status: 'passed' }, { name: 'external_origin_tls_and_identity', status: 'pending', code: 'server_access_pending_until_apply' }];
      for (const [id, test] of this.tests) if (test.expires <= this.now()) this.tests.delete(id);
      while (this.tests.size >= 8) this.tests.delete(this.tests.keys().next().value);
      const testId = createHash('sha256').update(JSON.stringify({ configHash, baseRevision: this.current.revision, expires, nonce: randomBytes(32).toString('hex') })).digest('hex');
      this.tests.set(testId, { configHash, baseRevision: this.current.revision, expires, managementOrigin: trusted.managementOrigin, checks });
      return { testId, baseRevision: this.current.revision, configHash, expiresAt: new Date(expires).toISOString(), scope: 'preflight', checks, plannedDockerPorts: this.dockerPorts(), requiresRestart: true };
    } catch (error) { throw httpError(error); }
    finally { this.testing = false; this.busy = false; }
  }
  // The journal of a change, written before anything else happens.
  begin({ operationId, config, publicIp, revision, checks = [], kind = 'save' }) {
    this.cancelRequested = false; this.committed = false;
    const previous = this.read(this.paths.policy, 32768);
    this.state = {};
    return this.set({ schema: 1, operationId, kind, state: 'waiting_idle', revision, config, publicIp, previousRevision: this.current.revision, previousConfig: this.current.config, previousPublicIp: this.current.publicIp, previous,
      checks, errorCode: null, rollbackVerified: false, unchangedVerified: false, idleReasons: [], blockers: [], waitUntil: null, starts: 0 });
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
  // Withdraws a save that still waits for the server's own work: nothing has
  // been ended or written yet.
  async cancel(input) {
    try {
      if (!exact(input, ['operationId']) || !uuid.test(input.operationId ?? '')) fail('command_invalid');
      if (!this.busy || this.state.state !== 'waiting_idle' || this.state.operationId !== input.operationId) fail('cancel_unavailable');
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
  async perform({ config, publicIp, revision }) {
    let written = false;
    try {
      // An administrator's save is made beside the .env lines of now; the
      // server's own change of the public address keeps what was remembered.
      const saving = this.state.kind !== 'public-ip', envSeed = saving ? this.envSeed : this.savedEnvSeed;
      const text = selfPolicyText(config, publicIp, this.now, envSeed);
      // Only whether the public IP is followed changes: the addresses in use
      // stay, so nobody is disconnected and nothing restarts.
      if (this.current.config && serverAccessHash(config) === this.current.healthRevision) {
        await ensureSelfIdentity(this.paths, this.read);
        written = true;
        await putSelfPolicy(this.paths, text);
        await this.finish({ state: 'succeeded', errorCode: null });
        this.current = { ...this.current, publicIp, revision, saved: true };
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
      const next = { config: selfAccessPolicy({ primaryOrigin: config.primaryOrigin, https: enabledOrigins(config.https), http: [...others, origin], trustedProxies: config.trustedProxies }, this.fixed), publicIp: { follow: true, origin } };
      next.revision = selfPolicyRevision(next.config, next.publicIp);
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
