import { createServer } from 'node:http';
import { createServer as createTlsServer } from 'node:https';
import { describeTransport } from './transport-policy.mjs';
import { randomUUID, createHash } from 'node:crypto';
import { readFile } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import { hashPassword, verifyPassword, newToken, tokenHash, username, desktopId, requestAddress, trustedProxies } from './security.mjs';
import { HttpError, fields } from './errors.mjs';
import { adminRoutes } from './admin-http.mjs';
import { Proxmox } from './proxmox.mjs';
import { validatePublicOrigin, validateDeploymentKind, readProtectedAccessFile } from './config.mjs';
import { renderProxmoxSetup } from './setup-script.mjs';
import { Relay } from './relay.mjs';
import { hostReadyProtocol } from './host-ready-wait.mjs';
import { downloadRoutes, readReleases } from './downloads.mjs';
import { releaseHistoryRoutes } from './release-history.mjs';
import { requestLanguage } from './i18n.mjs';
import { prefersHtml, renderLanding, renderDownloadPage, renderMissingRelease, latestFile } from './landing.mjs';
import { serverUpdateDownloads } from './server-update-downloads.mjs';
import { BackupService } from './backup.mjs';
import { createReadStream } from 'node:fs';
import { activitySnapshot, localActivityRequest, localDesktopActivityTarget, desktopActivitySnapshot, connectionCounts, endConnections } from './activity.mjs';
import { PowerControl } from './power.mjs';
import { VmRelocation } from './vm-relocate.mjs';
import { PcPower } from './pc-power.mjs';
import { VmAudioControl, vmAudioProtocol } from './vm-audio.mjs';
import { VmStates } from './vm-state.mjs';
import { DesktopEvents } from './desktop-events.mjs';
import { CatalogWatch } from './catalog-watch.mjs';
import { HostRequests } from './host-requests.mjs';
import { receiveClientDiagnostics } from './client-diagnostics.mjs';
import { HostUpdates } from './host-update.mjs';
import { InitialSetupCode } from './initial-setup.mjs';
import { resolveRequestAccess, bootstrapPath, legacyProxyHealthProbe } from './request-origin.mjs';
import { observabilityTls } from './stream-observability.mjs';
import { AccessRuntime } from './access-runtime.mjs';
import { serverAccessHash, endpointOrigins } from '../update/server-access-policy.mjs';
import { BrowserConsole, consoleHttpsPage, consoleHttpsPagePolicy, refusalPage } from './browser-console.mjs';
import { RetentionSweeper } from './retention.mjs';
import { UsageRecorder, noteLogin, noteClientReport } from './usage.mjs';
import { clientSessionPolicy, pcSessionPolicy } from './client-session-policy.mjs';
import { requestedSessionContext, existingLoginView } from './session-context.mjs';
import { SessionAccess, SessionAccessError, sessionAccessProtocol } from './session-access.mjs';
import { createSessionAccessStore } from './session-access-store.mjs';
import { MfaProvider } from './mfa.mjs';
import { NoticeService } from './notices.mjs';
import { AdminResources, ServerResources } from './admin-resources.mjs';

export const VERSION = JSON.parse(readFileSync(new URL('../package.json', import.meta.url), 'utf8')).version;
// Pages and their static assets: same-origin scripts, styles and images only;
// no inline script or style, no external fonts or CDNs.
const pagePolicy = "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; img-src 'self'; font-src 'self'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";
const staticAssets = [
  ['/admin/access.css', 'access.css', 'text/css'],
  ['/admin/account-security.css', 'account-security.css', 'text/css'], ['/admin/usage.css', 'usage.css', 'text/css'], ['/admin/notices.css', 'notices.css', 'text/css'], ['/admin/backup.css', 'backup.css', 'text/css'],
  ['/admin/server-access.css', 'server-access.css', 'text/css'], ['/admin/resources.css', 'resources.css', 'text/css'],
  ['/admin/product-ui.css', 'product-ui.css', 'text/css'],
  ['/admin', 'index.html', 'text/html'], ['/admin/', 'index.html', 'text/html'], ['/admin/app.js', 'app.js', 'text/javascript'],
  ['/admin/help/proxmox-role-add.png', 'help/proxmox-role-add.png', 'image/png'], ['/admin/help/LICENSE-proxmox.txt', 'help/LICENSE-proxmox.txt', 'text/plain'], ['/admin/licenses/ws-LICENSE.txt', 'licenses/ws-LICENSE.txt', 'text/plain'], ['/admin/licenses/nodejs-LICENSE.txt', 'licenses/nodejs-LICENSE.txt', 'text/plain'], ['/admin/licenses/Multitap-AGPL-3.0.txt', 'licenses/Multitap-AGPL-3.0.txt', 'text/plain'], ['/admin/licenses/noVNC-LICENSE.txt', 'licenses/noVNC-LICENSE.txt', 'text/plain'],
  ['/admin/console.css', 'console.css', 'text/css'], ['/admin/pages.css', 'pages.css', 'text/css'], ['/admin/proxmox-shell-setup.js', 'proxmox-shell-setup.js', 'text/javascript'],
  ['/admin/base.css', 'base.css', 'text/css'], ['/admin/home.css', 'home.css', 'text/css'], ['/admin/theme.js', 'theme.js', 'text/javascript'], ['/admin/icons.js', 'icons.js', 'text/javascript'],
  ['/admin/downloads.css', 'downloads.css', 'text/css'], ['/admin/home.js', 'home.js', 'text/javascript'], ['/admin/download-choices.js', 'download-choices.js', 'text/javascript'],
  ['/admin/workspace.css', 'workspace.css', 'text/css'], ['/admin/operations.css', 'operations.css', 'text/css'], ['/admin/server-update-badge.css', 'server-update-badge.css', 'text/css'],
  ['/admin/js/server-update-badge.js', 'js/server-update-badge.js', 'text/javascript'], ['/admin/fonts/PretendardVariable.woff2', 'fonts/PretendardVariable.woff2', 'font/woff2'], ['/admin/fonts/LICENSE.txt', 'fonts/LICENSE.txt', 'text/plain'],
  // Language (docs/i18n.md): the choice before the first paint, and the text catalogs js/i18n.js imports.
  ['/admin/lang.js', 'lang.js', 'text/javascript'], ['/admin/i18n/ko.js', 'i18n/ko.js', 'text/javascript'], ['/admin/i18n/en.js', 'i18n/en.js', 'text/javascript'],
  ['/admin/mark.svg', 'mark.svg', 'image/svg+xml'], ['/admin/favicon.svg', 'favicon.svg', 'image/svg+xml'], ['/admin/scene-dusk.jpg', 'scene-dusk.jpg', 'image/jpeg'], ['/admin/scene-daylight.jpg', 'scene-daylight.jpg', 'image/jpeg'],
  // Console modules imported by app.js (same origin, so script-src 'self' covers them).
  ...['dom', 'labels', 'charts', 'perf', 'timeline', 'permissions', 'hosts', 'desktops', 'users', 'monitor', 'usage', 'resources', 'overview', 'system', 'downloads', 'server-access', 'backup', 'notices', 'mfa-auth', 'account-security-controller', 'account-security-view', 'i18n'].map(name => [`/admin/js/${name}.js`, `js/${name}.js`, 'text/javascript']),
];
const transportSecurity = 'max-age=31536000';
export class Limiter {
  constructor(limit, windowMs = 60000, max = 10000) { this.limit = limit; this.windowMs = windowMs; this.max = max; this.entries = new Map(); }
  take(key, now) {
    let entry = this.entries.get(key);
    if (!entry || entry.until <= now) {
      if (!entry && this.entries.size >= this.max && !this.makeRoom(now)) return false;
      entry = { count: 0, until: now + this.windowMs }; this.entries.set(key, entry);
    }
    entry.count++;
    return entry.count <= this.limit;
  }
  // A full table drops expired entries, then the oldest key still under its
  // limit. A key that is currently locked out is never forgotten: spraying
  // fresh keys used to evict it and reset its failed-attempt count.
  makeRoom(now) {
    for (const [key, value] of this.entries) if (value.until <= now) this.entries.delete(key);
    if (this.entries.size < this.max) return true;
    for (const [key, value] of this.entries) if (value.count < this.limit) { this.entries.delete(key); return true; }
    return false;
  }
}
async function body(req, max = 8192) {
  if ((req.headers['content-type'] ?? '').split(';')[0].trim() !== 'application/json') throw new HttpError(415, 'json_required');
  if (Number(req.headers['content-length']) > max) { req.resume(); throw new HttpError(413, 'body_too_large'); }
  const raw = await new Promise((resolve, reject) => {
    let length = 0; const chunks = [];
    const cleanup = () => { req.off('data', data); req.off('end', end); req.off('error', error); req.off('aborted', aborted); };
    const error = err => { cleanup(); reject(err); };
    const aborted = () => error(new HttpError(400, 'request_aborted'));
    const data = chunk => {
      length += chunk.length;
      if (length > max) { cleanup(); req.resume(); reject(new HttpError(413, 'body_too_large')); }
      else chunks.push(chunk);
    };
    const end = () => { cleanup(); resolve(Buffer.concat(chunks).toString('utf8')); };
    req.on('data', data); req.once('end', end); req.once('error', error); req.once('aborted', aborted);
  });
  let parsed;
  try { parsed = JSON.parse(raw); } catch { throw new HttpError(400, 'invalid_json'); }
  if (!parsed || Array.isArray(parsed) || typeof parsed !== 'object') throw new HttpError(400, 'invalid_json_object');
  return parsed;
}
const publicUser = user => ({ id: user.id, username: user.username, displayName: user.username, role: user.role });
// Power refusals decided before Proxmox is asked (audited as power_refused).
// Rate-limited attempts are not audited: a looping client must not grow the
// audit table without bound.
const powerRefusals = new Set(['invalid_power_action', 'power_busy', 'power_in_progress', 'power_status_unavailable', 'invalid_power_state', 'display_in_progress', 'host_update_in_progress', 'pc_wake_unavailable', 'pc_power_unsupported']);
function accessFailure(error) {
  if (!(error instanceof SessionAccessError)) return error;
  if (error.code === 'login_confirmation_expired') return new HttpError(410, error.code);
  if (['login_confirmation_stale','login_confirmation_invalid'].includes(error.code)) return new HttpError(409, 'login_confirmation_changed');
  if (error.code === 'active_session_limit') return new HttpError(409, error.code);
  if (error.code === 'login_confirmation_limit') return new HttpError(429, 'too_many_attempts');
  if (['session_idle_timeout','session_expired'].includes(error.code)) return new HttpError(401, error.code);
  return new HttpError(503, 'session_access_unavailable');
}

export async function buildServer({ store, vault, proxmox = new Proxmox(), publicOrigin, allowInsecureHttp = false, tls = null, accessConfig = undefined, accessRevision = null, accessInstallationId = null, accessIdentityFile = null, accessIdentityRead = null, accessOperationId = null, legacyAdminOrigin = null, bootstrap = false, setupOrigin = null, accessClient = null, restoreClient = null, backupDir = null, backupOptions = {}, deploymentKind = 'unknown', dataDir = null, downloadDir, serverUpdate = null, restartServer = null, sessionTtlMs = 8 * 60 * 60 * 1000, sessionMaxLifetimeMs = 7 * 24 * 60 * 60 * 1000, loginLimit = 8, addressLimit = 60, trustedProxies: proxies, initialSetupOptions = {}, relayOptions = {}, browserConsoleOptions = {}, powerOptions = {}, relocationOptions = {}, resourceOptions = {}, pcPowerOptions = {}, desktopEventOptions = {}, vmStateOptions = {}, catalogWatchOptions = {}, hostRequestOptions = {}, hostUpdateOptions = {}, sessionAccessOptions = {}, mfaOptions = {}, certificateMonitor = null, logger = console }) {
  const strictAccess = accessConfig !== undefined || bootstrap;
  // A fresh installation has only its management listener, not a fabricated
  // public domain. Legacy direct API callers retain their negotiated profile.
  publicOrigin = publicOrigin === null && bootstrap ? 'http://localhost' : validatePublicOrigin(publicOrigin, { allowInsecureHttp });
  if (bootstrap && !accessConfig) allowInsecureHttp = true;
  const transport = describeTransport(publicOrigin, { allowInsecureHttp, directTls: tls !== null });
  const accessRuntime = new AccessRuntime({ client: accessClient, config: accessConfig ?? null, revision: accessRevision, installationId: accessInstallationId, initialOperationId: accessOperationId });
  deploymentKind = validateDeploymentKind(deploymentKind);
  const proxyTrust = trustedProxies(proxies);
  // 백업·복원: the first start after a restore turns the marker the restored
  // database carries into the record and audit row of that restore.
  const backup = new BackupService({ store, vault, directory: backupDir, version: VERSION, origin: bootstrap && !accessConfig ? null : publicOrigin, accessConfig: accessConfig ?? null, deploymentKind, restoreClient, accessRuntime, proxmox, ...backupOptions });
  backup.startup();
  if (backup.available) void backup.prepare().catch(() => {});
  // A fresh server prints its one-time setup code to its own log now.
  const initialSetup = new InitialSetupCode(store, initialSetupOptions); initialSetup.startup();
  const dummyHash = await hashPassword(newToken());
  const accounts = new Limiter(loginLimit);
  const addresses = new Limiter(addressLimit);
  const enrollmentAttempts = new Limiter(20);
  const powerAttempts = new Limiter(10);
  const audioAttempts = new Limiter(10);
  const displayAttempts = new Limiter(10);
  const takeoverAttempts = new Limiter(10);
  const hostUpdateAttempts = new Limiter(20);
  const favoriteAttempts = new Limiter(60);
  // desktop-preview-v1: about one read per assigned desktop a minute, plus refreshes.
  const previewAttempts = new Limiter(300);
  const relay = new Relay({ store, vault, proxmox, publicOrigin, ...relayOptions, brokerVersion: VERSION });
  // A registered VM that moved (migration, HA recovery, restore to a new VM
  // ID) is found again by its UUID (vm-relocate.mjs): inventory and power
  // refreshes, Console and the stream console proof share one lookup.
  const vmRelocation = new VmRelocation({ store, proxmox, ...relocationOptions });
  relay.vmRelocation = vmRelocation;
  relay.assertAccessAdmission = () => accessRuntime.assertAdmission();
  relay.accessPending = () => accessRuntime.blocked;
  // 모니터링 (usage.mjs): connection start and end as the relay sees them.
  const usage = new UsageRecorder({ store, relay }).start();
  relay.usage = usage;
  // Old diagnostic and audit rows go in small batches, preferably while no remote session runs.
  const retention = new RetentionSweeper({ store, idle: () => relay.sessions.size === 0 }).start();
  const browserConsole = new BrowserConsole({ store, vault, relay, sessionTtlMs, sessionMaxLifetimeMs, ...browserConsoleOptions });
  const hostRequests = new HostRequests({ store, vault, proxmox, changed: () => relay.revalidate(), ...hostRequestOptions });
  const power = new PowerControl({ store, vault, proxmox, relocation: vmRelocation, ...powerOptions, mutationGuard: id => relay.assertPowerAllowed(id) });
  let desktopEvents;
  // 공지 보내기 (notices.mjs): a send or a withdrawal reaches the clients that
  // asked for it over the channels they already hold.
  const notices = new NoticeService({ store, relay, changed: users => { desktopEvents?.noticesChanged(users); relay.noticesChanged(users); } });
  const sessionClock = sessionAccessOptions.now ?? Date.now;
  const sessionAuthority = createSessionAccessStore(store, { now: sessionClock });
  const sessionAccess = new SessionAccess({ ...sessionAccessOptions, authority: sessionAuthority.authority });
  const accessGuard = hash => sessionAccess.enforceCurrent(hash);
  store.sessionAccessGuard = accessGuard;
  const pendingLoginContexts = new Map();
  const mfa = new MfaProvider({ store, vault, now: sessionClock, ...mfaOptions, changed: () => relay.revalidate() });
  const acceptHumanInput = (hash, userId, kind) => sessionAccess.recordAcceptedInput(sessionAuthority.mintInputProof(hash, userId, kind));
  relay.sessionInputAccepted = session => {
    try { acceptHumanInput(session.tokenHash, session.userId, 'remote-input'); return true; }
    catch { return false; }
  };
  relay.consoleInputAccepted = value => {
    try {
      // A WebView's derived principal is scoped to this origin and desktop;
      // only its server-owned, currently authenticated parent owns activity.
      const user = relay.consoleAuthenticate?.(value.tokenHash, value.desktopId, value.origin);
      const parentHash = relay.consoleParentHash?.(value.tokenHash);
      if (!user || user.id !== value.userId || user.must_change_credentials || !store.assignedDesktop(user.id, value.desktopId)) return false;
      acceptHumanInput(parentHash, user.id, 'remote-input'); return true;
    } catch { return false; }
  };
  const idleSweep = setInterval(() => {
    try {
      const result = sessionAccess.sweepIdle();
      if (result.ended.length) { relay.revalidate(); desktopEvents?.notify(); }
    } catch { /* Authentication remains guarded if the observer is unavailable. */ }
  }, 1000); idleSweep.unref();
  // Approved physical PCs: 종료/다시 시작 through their Host, 켜기 by Wake-on-LAN.
  const pcPower = new PcPower(relay, { onChange: () => desktopEvents?.notify(), ...pcPowerOptions });
  relay.pcPower = pcPower; power.pc = pcPower;
  relay.powerBusy = id => !!power.active(id) || pcPower.busy(id);
  // A sound-card state change reaches clients as an ordinary list change.
  const vmAudio = new VmAudioControl({ store, vault, proxmox, relay, onChange: () => desktopEvents?.notify() });
  relay.vmConfigOperations = vmAudio.operations;
  // Proxmox's latest answer about the integration's VM.Console at /vms: true,
  // false, or null while not asked since this start or since the Proxmox
  // settings changed. The administrator console shows it (admin-http.mjs); the
  // console relay asks Proxmox itself before every console.
  let consolePrivilege = null;
  relay.vmConfigBusy = id => vmAudio.operations.has(id);
  // The VM state machine: every signal (power tasks, Proxmox status, guest
  // agent, Host events) ends in a push through the live channel.
  // resourceMs: how often the one /cluster/resources read (CPU, RAM, uptime of
  // every VM) is taken while at least one client keeps the live channel open;
  // syncMs otherwise. Changed values are pushed, not re-read by clients.
  const { syncMs: desktopSyncMs = 45000, resourceMs = 5000, ...vmStateConfig } = vmStateOptions;
  const vmStates = new VmStates({ store, power, relay, onChange: () => desktopEvents?.notify(), ...vmStateConfig });
  power.hostReport = id => relay.hostResources(id);
  // 홈 자원 (admin-resources.mjs): the server, its process, container or service, its data directory and the running desktops, for administrators only.
  const adminResources = new AdminResources({ store, power, relay, server: new ServerResources({ dataDir, deploymentKind, ...resourceOptions }) });
  relay.onHostEvent = (id, kind, detail) => { pcPower.hostEvent(id, kind); vmStates.hostEvent(id, kind, detail); };
  {
    const submit = power.submit.bind(power);
    power.submit = async (authorize, actor, action, options) => {
      let id = null;
      const result = await submit(() => { const desktop = authorize(); id = desktop.id; return desktop; }, actor, action, options);
      if (id) { vmStates.powerEvent(id); vmAudio.markDue(id); }
      return result;
    };
  }
  relay.hostUpdate = new HostUpdates(relay, { downloadDir, onChange: () => desktopEvents?.notify(), ...hostUpdateOptions });
  // A save of the server access settings is no longer refused while a Host
  // update runs: the engine waits for one already sent to its Host, shows
  // that on the page and can be cancelled (server-access-engine.mjs). One
  // still waiting for its connection to end is not sent while the fence is
  // up (HostUpdates.busy) and, like any in-memory request, ends with the
  // restart.
  // Every account sees the Host version and remote-update state of the VMs
  // assigned to it (it could install the same update by connecting anyway).
  // hostMissingAfterUpdate (only while true; released clients ignore it): the
  // Host went away with the last update attempt and stayed away while the
  // desktop runs. The live channel pushes it as an ordinary list change.
  async function withHostUpdates(desktops, viewer) {
    const { desktops: views } = await relay.hostUpdate.views(desktops.map(desktop => desktop.id));
    return desktops.map(desktop => {
      const missing = relay.hostUpdate.missingAfterUpdate(desktop);
      return { ...desktop, hostUpdate: views[desktop.id], ...(missing ? { hostMissingAfterUpdate: missing } : {}), ...(desktop.kind === 'vm' ? { vmAudioProtocol, soundCardAddAllowed: false, ...withAudio(desktop.id, viewer) } : {}) };
    });
  }
  // vm-audio-v1 state for the list; omitted while unknown (a client shows no badge).
  // soundCardAddAllowed is false for every VM: released clients then do not offer
  // their sound-card dialog (vm-audio.mjs).
  function withAudio(id, viewer) { const audio = vmAudio.listView(id, viewer); return audio ? { audio } : {}; }
  async function desktopSnapshot(hash, { refresh = true, origin = publicOrigin, signinProtocol } = {}) {
    if (refresh) { await power.refresh(); void vmAudio.refreshStale(); }
    const current = store.authenticate(hash);
    if (!current) throw new HttpError(401, 'session_expired');
    if (current.must_change_credentials) throw new HttpError(428, 'credentials_change_required');
    // The live channel reads without asking Proxmox, so its freshness bound is
    // the background sync, not the short request cache.
    const decorated = power.decorate(relay.decorate(store.listDesktops(current.id), current.id, origin), current.id, refresh ? {} : { staleMs: desktopSyncMs * 2 + 10000 });
    const views = await withHostUpdates(vmStates.decorate(decorated), current);
    return signinProtocol === 'signin-stream-v1' ? relay.withSigninAvailability(views, hash, current.id) : views;
  }
  let powerTaskCursor = 0;
  async function refreshDesktopObservations(userIds) {
    // Slow background sync for changes made outside Multitap; watched power
    // changes arrive per VM (vm-state.mjs) and Host events instantly.
    await power.refresh({ maxAgeMs: desktopEvents.watching ? Math.min(resourceMs, desktopSyncMs) : desktopSyncMs });
    // Sound-card states refresh beside it, never inside the watchers' time limit.
    void vmAudio.refreshStale();
    if (!userIds.length) return;
    const rows = store.db.prepare(`SELECT DISTINCT p.* FROM power_tasks p JOIN assignments a ON a.desktop_id=p.desktop_id WHERE p.active=1 AND a.user_id IN (${userIds.map(() => '?').join(',')}) ORDER BY p.id`).all(...userIds);
    if (!rows.length) { powerTaskCursor = 0; return; }
    const batch = Array.from({ length: Math.min(16, rows.length) }, (_, index) => rows[(powerTaskCursor + index) % rows.length]);
    powerTaskCursor = (powerTaskCursor + batch.length) % rows.length;
    await Promise.allSettled(batch.map(row => power.poll(row)));
  }
  desktopEvents = new DesktopEvents({ store, publicOrigin, refresh: refreshDesktopObservations, snapshot: hash => desktopSnapshot(hash, { refresh: false }), ...desktopEventOptions });
  // A published/replaced release shows at once: the Host update view forgets
  // its short catalog cache, watchers re-read their list and ask clients to
  // re-read the public catalog (update badges).
  const catalogWatch = new CatalogWatch({ directory: downloadDir, onChange: () => { relay.hostUpdate.cache = null; desktopEvents.catalogChanged(); desktopEvents.notify(); }, ...catalogWatchOptions });
  const serveDownload = downloadRoutes(downloadDir);
  const serveServerUpdate = serverUpdateDownloads(downloadDir);
  const serveReleaseHistory = releaseHistoryRoutes(downloadDir);
  let passwordJobs = 0;
  async function passwordJob(action) {
    if (passwordJobs >= 2) throw new HttpError(503, 'login_busy');
    passwordJobs++;
    try { return await action(); } finally { passwordJobs--; }
  }
  const assets = new Map();
  const clientDiagnosticAttempts = new Limiter(60);
  for (const [path, file, mime] of staticAssets) {
    assets.set(path, { bytes: await readFile(new URL('../admin/' + file, import.meta.url)), mime });
  }
  // One template for the whole run: the bytes served at /setup/proxmox.py and
  // the SHA-256 the console puts in the setup command come from the same text.
  const proxmoxTemplate = await readFile(new URL('../setup/proxmox.py', import.meta.url), 'utf8');
  const proxmoxSetupBytes = origin => Buffer.from(renderProxmoxSetup(proxmoxTemplate, origin, { allowInsecureHttp: origin.startsWith('http:') }), 'utf8');
  const proxmoxSetupSha256 = origin => { try { return createHash('sha256').update(proxmoxSetupBytes(origin)).digest('hex'); } catch { return null; } };
  // Public pages for people (landing.mjs): GET /download, the short links
  // /get/client, /get/host and /get/android that always lead to the newest
  // published file (short enough to type inside a fresh VM), and /downloads
  // for a browser. /downloads/<file> and /api/v1/downloads are unchanged.
  // HSTS applies to every port of a hostname. An administrator's explicit
  // same-host HTTP opt-in must clear our prior HTTPS-only policy over HTTPS;
  // never let a header silently turn the configured HTTP listener into TLS.
  const originHeaders = origin => origin.startsWith('https:') ? {
    'Strict-Transport-Security': strictAccess && accessConfig?.http.enabled &&
      endpointOrigins(accessConfig.http).some(value => new URL(value).hostname === new URL(origin).hostname) ? 'max-age=0' : transportSecurity,
  } : {};
  const htmlHeaders = (requestId, origin) => ({ 'Content-Type': 'text/html; charset=utf-8', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'Content-Security-Policy': pagePolicy, ...originHeaders(origin), 'X-Request-Id': requestId });
  const shortLinks = new Set(['client', 'host', 'android']);
  async function servePublicPage(req, res, path, requestId, publicOrigin) {
    const securityHeaders = originHeaders(publicOrigin);
    if (req.method !== 'GET' && req.method !== 'HEAD') return false;
    // Typed addresses: any letter case and an optional trailing slash.
    const typed = path.length <= 32 ? path.toLowerCase().replace(/(.)\/$/, '$1') : '';
    const redirect = location => {
      res.writeHead(302, { Location: location, 'Cache-Control': 'no-store', 'Content-Length': 0, 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'", ...securityHeaders, 'X-Request-Id': requestId });
      res.end(); return true;
    };
    const html = (status, body, vary) => { res.writeHead(status, { ...htmlHeaders(requestId, publicOrigin), ...(vary ? { Vary: vary } : {}) }); res.end(req.method === 'HEAD' ? undefined : body); return true; };
    if (typed.startsWith('/get/') && shortLinks.has(typed.slice(5))) {
      const key = typed.slice(5), file = latestFile(await readReleases(downloadDir), key);
      res.setHeader('Vary', 'Accept, Accept-Language');
      if (file) return redirect(file.url);
      // Nothing published yet: a page for a browser, a JSON error otherwise.
      if (prefersHtml(req.headers.accept)) return html(404, renderMissingRelease({ version: VERSION, key, lang: requestLanguage(req.headers['accept-language']) }));
      throw new HttpError(404, 'release_not_found');
    }
    if (typed === '/download') return html(200, renderDownloadPage({ version: VERSION, publicOrigin, releases: await readReleases(downloadDir), userAgent: req.headers['user-agent'], lang: requestLanguage(req.headers['accept-language']) }), 'User-Agent, Accept-Language');
    if (path === '/downloads' || path === '/downloads/') {
      res.setHeader('Vary', 'Accept');
      if (prefersHtml(req.headers.accept)) return redirect('/download');
    }
    return false;
  }
  const brokerStartedAt = Date.now();
  // What a save of the access settings reads and ends. The updater asks over
  // the fenced loopback routes below; a self-managed Docker install asks in
  // this process and takes the same fence itself (self-access.mjs).
  const accessActivity = () => ({ protocol: 'server-access-activity-v1', fence: true, activity: activitySnapshot({ relay, desktopEvents, store, startedAt: brokerStartedAt }), connections: connectionCounts(relay) });
  const endAccessConnections = () => {
    const operation = accessRuntime.connectionsEnding();
    if (!operation) return null;
    const ended = endConnections(relay);
    if (ended.sessions + ended.consoles > 0) {
      try { store.audit(operation.event, operation.actor, `${operation.operationId}:${ended.people}:${ended.sessions + ended.consoles}`); } catch { /* Audit I/O cannot keep connections open. */ }
    }
    return { protocol: 'server-access-connections-v1', fence: true, ended };
  };
  accessClient?.attach?.({ activity: accessActivity, endConnections: endAccessConnections, hold: options => accessRuntime.hold(options), release: lease => accessRuntime.release(lease),
    blocked: () => accessRuntime.blocked, settle: () => accessRuntime.status({ managementOrigin: null }) });
  // At most one re-check per event-loop turn, however many responses ask.
  let revalidateQueued = false, notifyQueued = false;
  const revalidateSoon = notify => {
    notifyQueued ||= notify;
    if (revalidateQueued) return;
    revalidateQueued = true;
    queueMicrotask(() => {
      const wake = notifyQueued; revalidateQueued = false; notifyQueued = false;
      try { relay.revalidate(); if (wake) desktopEvents.notify(); }
      catch (error) { logger.error(JSON.stringify({ event: 'revalidate_error', type: error?.name })); }
    });
  };
  const requestListener = async (req, res) => {
    const requestId = randomUUID();
    const preparationRead = browserConsole.isPreparationRead(req);
    // Why a browser's page request was refused, when the code alone does not say (http_disabled before the first setup).
    let refusalKind = null;
    let securityHeaders = browserConsole.isPath((req.url ?? '').split('?')[0]) ? { 'Referrer-Policy': 'no-referrer' } : {};
    const send = (status, value, notify = true) => {
      res.writeHead(status, { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'", ...securityHeaders, 'X-Request-Id': requestId, ...(status === 429 ? { 'Retry-After': '60' } : {}) });
      res.end(JSON.stringify(value));
      // Only a successful request that can change state re-checks the live
      // channels and wakes the desktop lists (logout, account, assignment and
      // Host changes end what they revoke at once); reads, refusals and
      // unknown paths do not, so a flood of them costs nothing here. The
      // periodic checks in relay.mjs and desktop-events.mjs cover the rest.
      if (!preparationRead && status >= 200 && status < 300 && !['GET', 'HEAD', 'OPTIONS'].includes(req.method)) revalidateSoon(notify);
    };
    try {
      const url = new URL(req.url, 'http://localhost');
      const path = url.pathname;
      // Update tooling on this machine asks what is running before it replaces
      // the broker (activity.mjs). Anyone else gets the ordinary 404.
      if (path === '/internal/activity' && req.method === 'GET' && !url.search && localActivityRequest(req)) { send(200, activitySnapshot({ relay, desktopEvents, store, startedAt: brokerStartedAt }), false); return; }
      if (path === '/internal/activity/desktop') {
        const target = localDesktopActivityTarget(req, url);
        const snapshot = target ? desktopActivitySnapshot({ relay, vmAudio, store }, target) : null;
        if (snapshot) { send(200, snapshot, false); return; }
        throw new HttpError(404, 'not_found');
      }
      if (path === '/internal/server-access/activity' && req.method === 'GET' && !url.search && localActivityRequest(req) && accessRuntime.accepts(req.headers['x-multitap-access-fence'])) { send(200, accessActivity(), false); return; }
      // Saving the server access settings ends the connections.
      // Only the holder of the current fence, on this
      // machine, may ask; the broker ends them itself and records how many.
      if (path === '/internal/server-access/end-connections' && req.method === 'POST' && !url.search && localActivityRequest(req) && accessRuntime.accepts(req.headers['x-multitap-access-fence'])) { send(200, endAccessConnections(), false); return; }
      const internalHealth = strictAccess && path === '/healthz' && req.method === 'GET' && !url.search && localActivityRequest(req);
      let context = { origin: publicOrigin, bootstrapOnly: false };
      if (strictAccess) {
        try { context = resolveRequestAccess(req, accessConfig, { bootstrap, setupOrigin }); }
        catch (error) { if (!internalHealth && !legacyProxyHealthProbe(req, accessConfig)) throw error; context = { origin: publicOrigin, bootstrapOnly: !accessConfig, healthProbeOnly: true }; }
      }
      const requestOrigin = context.origin;
      req.brokerOrigin = requestOrigin;
      req.brokerTls = observabilityTls(req, context);
      securityHeaders = { ...securityHeaders, ...originHeaders(requestOrigin) };
      // A fresh install's root address opens the first-setup page, so nobody
      // has to type /admin: always while bootstrapping, and for a browser while
      // the initial administrator setup is still pending.
      if (path === '/' && !url.search && (req.method === 'GET' || req.method === 'HEAD') && !context.healthProbeOnly &&
          (context.bootstrapOnly || (prefersHtml(req.headers.accept) && initialSetup.pending()))) {
        res.writeHead(302, { Location: '/admin/', 'Cache-Control': 'no-store', 'Content-Length': 0, 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'", Vary: 'Accept', ...securityHeaders, 'X-Request-Id': requestId });
        res.end(); return;
      }
      if (context.bootstrapOnly && !bootstrapPath(path)) { refusalKind = 'setup_pending'; throw new HttpError(403, 'http_disabled'); }
      if (browserConsole.isPath(path) || /^\/console(?:\/|\?|$)/.test(req.url)) {
        securityHeaders['Referrer-Policy'] = 'no-referrer';
        try { browserConsole.validate(req, requestOrigin); } catch (error) {
          // A person who opened the page over plain HTTP gets a page that says
          // so. The API, the assets and every other refusal answer as before.
          if (error?.code !== 'console_https_required' || !['GET', 'HEAD'].includes(req.method) || browserConsole.isApiPath(path) || path.startsWith('/console/assets/') || !/(?:^|,)\s*text\/html\s*(?:;|,|$)/i.test(String(req.headers.accept ?? ''))) throw error;
          const page = consoleHttpsPage(requestLanguage(req.headers['accept-language']));
          res.writeHead(403, { 'Content-Type': 'text/html; charset=utf-8', 'Content-Length': page.length, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Content-Security-Policy': consoleHttpsPagePolicy, Vary: 'Accept, Accept-Language', ...securityHeaders, 'X-Request-Id': requestId });
          res.end(req.method === 'HEAD' ? undefined : page); return;
        }
      }
      const currentTransport = context.bootstrapOnly ? { protocol: 'broker-transport-v1', publicOrigin: requestOrigin, scheme: 'http', allowInsecureHttp: false, directTls: false } : describeTransport(requestOrigin, { allowInsecureHttp: requestOrigin.startsWith('http:'), directTls: req.socket.encrypted === true });
      if (accessRuntime.blocked && !['GET', 'HEAD', 'DELETE'].includes(req.method) && /\/(?:host-updates?|power|display|console-preparation|console-sessions)(?:\/|$)|\/server-update\/apply$/.test(path)) accessRuntime.assertAdmission();
      if (path === '/setup/proxmox.py' && req.method === 'GET') {
        const bytes = proxmoxSetupBytes(requestOrigin);
        res.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8', 'Content-Length': bytes.length, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Content-Security-Policy': "default-src 'none'; sandbox; frame-ancestors 'none'", 'Referrer-Policy': 'no-referrer', ...securityHeaders }); res.end(bytes); return;
      }
      if (await serveServerUpdate(req, res, path) || await servePublicPage(req, res, path, requestId, requestOrigin) || await serveDownload(req, res, path)) return;
      if (await serveReleaseHistory(req, res, url)) return;
      if (await browserConsole.serve(req, res, path, { origin: requestOrigin, headers: { ...securityHeaders, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'X-Request-Id': requestId } })) return;
      if (req.method === 'GET' && assets.has(path)) {
        const asset = assets.get(path);
        res.writeHead(200, { 'Content-Type': asset.mime + (asset.mime.startsWith('text/') ? '; charset=utf-8' : ''), 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'Content-Security-Policy': pagePolicy, ...securityHeaders });
        res.end(asset.bytes); return;
      }
      if (req.method === 'GET' && (path === '/' || path === '/healthz')) {
        store.db.prepare('SELECT 1').get();
        // /healthz is always JSON. / answers JSON too unless the request
        // explicitly prefers text/html (a browser navigation); see landing.mjs.
        // The page orders its download cards by the visitor's platform.
        if (path === '/') res.setHeader('Vary', 'Accept, User-Agent, Accept-Language');
        if (path === '/' && prefersHtml(req.headers.accept)) {
          const page = renderLanding({ version: VERSION, publicOrigin: requestOrigin, releases: await readReleases(downloadDir), userAgent: req.headers['user-agent'], lang: requestLanguage(req.headers['accept-language']) });
          res.writeHead(200, htmlHeaders(requestId, requestOrigin));
          res.end(page); return;
        }
        if (strictAccess && accessInstallationId === null && accessIdentityFile) {
          // An updater upgraded after this broker may create its protected
          // installation identity. Observe that one file without restarting
          // an active broker; once accepted, this process keeps the same ID.
          // A self-managed Docker install reads its own file the same way
          // (`accessIdentityRead`): access-cli.mjs makes it with a first policy
          // while the server runs, and the container's health check compares
          // this answer with that file.
          try {
            const value = accessIdentityRead ? accessIdentityRead() : readProtectedAccessFile(accessIdentityFile, { maximum: 128, production: true });
            if (value !== null) {
              if (!/^[a-f0-9]{64}$/.test(value.trim())) throw Error();
              accessInstallationId = value.trim(); accessRuntime.installationId = accessInstallationId;
            }
          } catch { throw new HttpError(503, 'server_access_identity_invalid'); }
        }
        // Public metadata reveals only this admitted origin. The revision is
        // computed from the running normalized policy, not a separately supplied
        // label; updater checks retain the policy hash without publishing its URLs.
        send(200, { service: 'multitap-broker', version: VERSION, status: 'ok', authentication: 'id-password', media: 'relay-v1', transport: strictAccess ? currentTransport : transport, ...(strictAccess ? { access: { protocol: 'server-access-v1', revision: accessConfig ? serverAccessHash(accessConfig) : (accessRevision ?? 'bootstrap'), origin: requestOrigin, installationId: accessInstallationId, bootstrapOnly: context.bootstrapOnly === true, ...(context.healthProbeOnly ? { healthProbeOnly: true } : {}) } } : {}) }); return;
      }
      if (!path.startsWith('/api/v1/') && !browserConsole.isApiPath(path)) throw new HttpError(404, 'not_found');
      if (url.search) throw new HttpError(400, 'query_not_supported');
      if (req.headers.origin && req.headers.origin !== requestOrigin) throw new HttpError(403, 'origin_not_allowed');
      const address = requestAddress(req, proxyTrust);
      const mfaContext = purpose => ({ origin: requestOrigin, purpose,
        secure: requestOrigin.startsWith('https://') && !context.bootstrapOnly &&
          (req.socket.encrypted === true || (strictAccess && context.proxied === true)) });
      const finishLogin = (user, admin, sessionPolicy, loginContextRequested) => {
        const token = newToken();
        const issuedAt = admin ? Date.now() : sessionClock();
        if (!admin) {
          try {
            const automatic = sessionPolicy.loginIntent === 'recovery-v1' || (sessionPolicy.issuer === 'legacy' && sessionPolicy.loginIntent === undefined);
            if (automatic && store.db.prepare('SELECT 1 FROM session_end_reasons WHERE user_id=? AND retain_until>? LIMIT 1').get(user.id, issuedAt)) {
              throw new HttpError(409, 'session_reauthentication_required');
            }
            const admission = sessionAccess.begin(user);
            if (admission.kind === 'confirmation-required') {
              if (sessionPolicy.loginIntent === 'recovery-v1' || sessionPolicy.loginIntent === 'interactive-v1') {
                sessionAccess.cancel(admission.challenge); throw new HttpError(409, 'pc_login_requires_interaction');
              }
              for (const [key, context] of pendingLoginContexts) if (context.until <= issuedAt) pendingLoginContexts.delete(key);
              const details = { protocol: sessionAccessProtocol, challengeToken: admission.challenge, expiresInMs: Math.max(0, Math.min(120000, Date.parse(admission.expiresAt) - sessionClock())) };
              if (loginContextRequested) {
                try { details.existingLogin = sessionAccess.describeConfirmation(admission.challenge, rows => existingLoginView(user, rows, row =>
                  [...relay.sessions.values()].filter(session => session.userId === user.id && session.tokenHash === row.tokenHash)
                    .map(session => store.assignedDesktop(user.id, session.desktopId)).filter(Boolean))); }
                catch (error) { sessionAccess.cancel(admission.challenge); throw error; }
              }
              pendingLoginContexts.set(tokenHash(admission.challenge), { origin: requestOrigin, issuer: sessionPolicy.issuer ?? 'legacy', userId: user.id, until: Date.parse(admission.expiresAt) });
              throw new HttpError(409, 'login_confirmation_required', 'login_confirmation_required', details);
            }
            sessionAccess.commitDirect(admission.admission, { tokenHash: tokenHash(token), ttlMs: sessionTtlMs, issuer: sessionPolicy.issuer ?? 'legacy' });
            noteLogin(store, tokenHash(token), { address });
            return { token, user, issuedAt, ttl: sessionTtlMs, issuer: sessionPolicy.issuer, sessionAccess: sessionAccess.describe(tokenHash(token)) };
          } catch (error) { throw accessFailure(error); }
        }
        let created;
        try {
          // No await between fresh live-owner observation and the synchronous
          // store transaction: parallel login cannot evict a newly attached VM.
          const protectedHashes = relay.protectedSessionHashes(user.id, issuedAt, desktopEvents?.clients.values() ?? []);
          created = store.createSession(user, tokenHash(token), issuedAt, sessionTtlMs, admin ? 'admin' : 'client', { protectedHashes, ...sessionPolicy });
        } catch (error) {
          if (error?.code === 'active_session_limit') throw new HttpError(409, 'active_session_limit');
          if (error?.code === 'pc_login_requires_interaction') throw new HttpError(409, 'pc_login_requires_interaction');
          throw error;
        }
        if (!created) throw new HttpError(401, 'invalid_credentials');
        return { token, user, issuedAt, ttl: sessionTtlMs };
      };
      const login = async (input, admin = false, issuerOptions = {}, loginContextRequested = false) => {
        // Console historically authenticates with purpose=client. Keep that
        // purpose and distinguish its server-owned issuer from PC policy.
        const sessionPolicy = admin ? { issuer: 'admin' } : issuerOptions === 'console' ? { issuer: 'browser-console' } : issuerOptions;
        let name;
        try { name = username(input.username); } catch { throw new HttpError(400, 'invalid_credentials_format'); }
        if (typeof input.password !== 'string' || input.password.length > 256 || !input.password) throw new HttpError(400, 'invalid_credentials_format');
        if (Object.keys(input).some(key => !['username', 'password'].includes(key))) throw new HttpError(400, 'unexpected_field');
        const now = Date.now();
        // The address first: a refused address must not spend (or create)
        // other accounts' budgets.
        if (!addresses.take(address, now) || !accounts.take(name, now)) throw new HttpError(429, 'too_many_attempts');
        const user = store.findUser(name);
        let valid;
        try { valid = await passwordJob(() => verifyPassword(input.password, user?.password_hash ?? dummyHash)); }
        finally { input.password = ''; }
        if (!valid || !user || user.disabled || (admin && user.role !== 'admin')) { store.audit('login_failed', name, ''); throw new HttpError(401, 'invalid_credentials'); }
        if (!admin && user.must_change_credentials) throw new HttpError(428, 'credentials_change_required');
        const fresh = store.findUser(name);
        if (!fresh || fresh.password_hash !== user.password_hash || fresh.authorization_revision !== user.authorization_revision || fresh.disabled || (admin && fresh.role !== 'admin')) throw new HttpError(401, 'invalid_credentials');
        if (mfa.enabled(fresh.id)) {
          if (req.headers['x-multitap-auth-protocol'] !== 'mfa-v1') throw new HttpError(428, 'mfa_client_update_required');
          return mfa.issue(fresh, mfaContext(admin ? 'admin' : issuerOptions === 'console' ? 'console' : 'client'), sessionPolicy, loginContextRequested);
        }
        return finishLogin(fresh, admin, sessionPolicy, loginContextRequested);
      };
      const confirmLogin = (input, issuer = 'native') => {
        fields(input, ['challengeToken','confirm']);
        if (input.confirm !== true || typeof input.challengeToken !== 'string' || !/^[A-Za-z0-9_-]{43}$/.test(input.challengeToken)) throw new HttpError(400, 'invalid_login_confirmation');
        const key = tokenHash(input.challengeToken), context = pendingLoginContexts.get(key);
        if (!context) throw new HttpError(409, 'login_confirmation_changed');
        if (context.origin !== requestOrigin) throw new HttpError(403, 'origin_not_allowed');
        // Console credentials may establish only the console cookie. Native
        // challenges cannot be consumed through that separate issuer either.
        if ((context.issuer === 'browser-console') !== (issuer === 'browser-console')) throw new HttpError(409, 'login_confirmation_changed');
        pendingLoginContexts.delete(key);
        if (context.until <= sessionClock()) { sessionAccess.cancel(input.challengeToken); throw new HttpError(410, 'login_confirmation_expired'); }
        const token = newToken();
        try { sessionAccess.confirm(input.challengeToken, { tokenHash: tokenHash(token), ttlMs: sessionTtlMs, issuer: context.issuer }); }
        catch (error) { throw accessFailure(error); }
        noteLogin(store, tokenHash(token), { address });
        const user = store.db.prepare('SELECT * FROM users WHERE id=?').get(context.userId);
        const currentSession = store.authenticate(tokenHash(token));
        if (!user || !currentSession) throw new HttpError(503, 'session_access_unavailable');
        return { token, user, expiresAt: new Date(currentSession.expires_at).toISOString(), issuer: context.issuer, sessionAccess: sessionAccess.describe(tokenHash(token)) };
      };
      const mfaRoutes = {
        guard: () => mfa.guard(mfaContext('console')),
        verify: (input, admin = false, issuerOptions = null) => {
          const purpose = admin ? 'admin' : issuerOptions === 'console' ? 'console' : 'client';
          return mfa.verify(input, mfaContext(purpose),
            (user, policy, contextual) => finishLogin(user, admin, policy, contextual),
            challenge => { sessionAccess.cancel(challenge); pendingLoginContexts.delete(tokenHash(challenge)); });
        },
        enabled: userId => mfa.enabled(userId),
        manage: async (action, input, principal) => {
          try { return await mfa.manage(action, input, { ...principal, ...mfaContext(principal.purpose) }, passwordJob); }
          finally { if (input) { input.currentPassword = ''; input.code = ''; input.recoveryCode = ''; input.setupToken = ''; } }
        },
      };
      // The standalone viewer shares password admission/session issuance. A
      // future MFA provider is passed here before any cookie can be issued.
      if (await browserConsole.handle(req, res, path, { origin: requestOrigin, body, send, login, confirmLogin, mfaRoutes })) return;
      if (await adminRoutes({ store, vault, proxmox, relay, power, hostRequests, notices, resources: adminResources, login, mfaRoutes, passwordJob, body, backup, stream: entry => { res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': entry.size, 'Content-Disposition': 'attachment; filename="' + entry.filename + '"', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'", ...securityHeaders, 'X-Request-Id': requestId }); const file = createReadStream(entry.path); file.once('error', () => res.destroy()); file.pipe(res); }, publicOrigin: requestOrigin, setupScriptSha256: () => proxmoxSetupSha256(requestOrigin), cookieScope: strictAccess ? requestOrigin : null, legacyCookieOrigin: legacyAdminOrigin, accessRuntime, restartServer, hostRemovalActivity: id => desktopActivitySnapshot({ relay, vmAudio, store }, id), serverUpdate, initialSetup, certificates: certificateMonitor, agentAccess: { denied: () => vmStates.agentRefused(), reset: () => vmStates.resetAgentDenial() }, audioInventory: (config, vms) => vmAudio.observeInventory(config, vms), consoleAccess: { state: () => consolePrivilege, record: granted => { consolePrivilege = typeof granted === 'boolean' ? granted : null; } }, relocation: vmRelocation, serverInfo: { service: 'multitap-broker', version: VERSION, transport: strictAccess ? currentTransport : transport, listener: req.socket.localPort ? { address: req.socket.localAddress, port: req.socket.localPort } : null, deployment: { kind: deploymentKind, configured: deploymentKind !== 'unknown' }, udpPort: relayOptions.udp?.port ?? null, startedAt: new Date(brokerStartedAt).toISOString(), restart: { supported: typeof restartServer === 'function' }, update: { state: 'not_configured', feedConfigured: false, applySupported: false } } })(req, res, path, send)) return;
      if (path === '/api/v1/hosts/requests' || path === '/api/v1/hosts/requests/status' || path === '/api/v1/hosts/requests/cancel') {
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        const input = await body(req, path === '/api/v1/hosts/requests' ? 2048 : 512);
        // Hosts that show the verification code say so in a header, which
        // released brokers ignore (a body field would be refused there).
        const showsCode = req.headers['x-multitap-host-request-code'] === 'v1';
        try {
          if (path.endsWith('/status')) send(200, hostRequests.status(input, address, showsCode));
          else if (path.endsWith('/cancel')) send(200, hostRequests.cancel(input, address));
          else send(201, hostRequests.create(input, address, showsCode));
        } finally { input.requestKey = ''; }
        return;
      }
      if (req.method === 'POST' && path === '/api/v1/hosts/enroll') {
        if (!enrollmentAttempts.take(address, Date.now())) throw new HttpError(429, 'too_many_attempts');
        const input = await body(req); fields(input, ['code', 'hostname', 'machineUuid', 'version']);
        const result = await relay.enroll(input); input.code = '';
        send(201, { ...result, serverOrigin: requestOrigin }); return;
      }
      if (req.method === 'POST' && path === '/api/v1/auth/login') {
        const policy = clientSessionPolicy(req);
        const loginContextRequested = requestedSessionContext(req, 'x-multitap-login-context');
        if (loginContextRequested && policy.loginIntent !== 'confirmable-v1') throw new HttpError(400, 'invalid_context_protocol');
        const session = await login(await body(req), false, policy, loginContextRequested);
        if (policy.issuer === pcSessionPolicy) securityHeaders['X-Multitap-Session-Policy'] = pcSessionPolicy;
        if (session.mfa) { send(202, session); return; }
        send(200, { sessionToken: session.token, expiresAt: new Date(session.issuedAt + session.ttl).toISOString(), user: publicUser(session.user), sessionAccess: session.sessionAccess }); return;
      }
      if (req.method === 'POST' && path === '/api/v1/auth/mfa/verify') {
        const input = await body(req, 1024);
        try {
          const session = mfaRoutes.verify(input);
          if (session.issuer === pcSessionPolicy) securityHeaders['X-Multitap-Session-Policy'] = pcSessionPolicy;
          send(200, { sessionToken: session.token, expiresAt: new Date(session.issuedAt + session.ttl).toISOString(), user: publicUser(session.user), sessionAccess: session.sessionAccess }); return;
        } finally { input.challengeToken = ''; input.code = ''; input.recoveryCode = ''; }
      }
      if (req.method === 'POST' && path === '/api/v1/auth/login/confirm') {
        const session = confirmLogin(await body(req, 1024));
        if (session.issuer === pcSessionPolicy) securityHeaders['X-Multitap-Session-Policy'] = pcSessionPolicy;
        send(200, { sessionToken: session.token, expiresAt: session.expiresAt, user: publicUser(session.user), sessionAccess: session.sessionAccess }); return;
      }
      const match = /^Bearer ([A-Za-z0-9_-]{43})$/.exec(req.headers.authorization ?? '');
      if (!match) throw new HttpError(401, 'authentication_required');
      const hash = tokenHash(match[1]);
      const user = store.authenticate(hash);
      if (!user) throw new HttpError(401, store.sessionRevocationReason(hash) ?? 'session_expired');
      if (user.must_change_credentials) throw new HttpError(428, 'credentials_change_required');
      const mfaMatch = /^\/api\/v1\/auth\/mfa(?:\/(setup|enable|reauth|disable|recovery))?$/.exec(path);
      if (mfaMatch) {
        const action = mfaMatch[1] ?? 'status';
        if (req.method !== (action === 'status' ? 'GET' : 'POST')) throw new HttpError(405, 'method_not_allowed');
        send(action === 'setup' ? 201 : 200, await mfaRoutes.manage(action, action === 'status' ? {} : await body(req, 1024), { user, hash, purpose: 'client', admin: false })); return;
      }
      if (path === '/api/v1/auth/idle' && req.method === 'PUT') {
        const input = await body(req, 1024); fields(input, ['enabled']);
        if (typeof input.enabled !== 'boolean') throw new HttpError(400, 'invalid_idle_policy');
        const current = store.authenticate(hash);
        if (!current || current.id !== user.id) throw new HttpError(401, store.sessionRevocationReason(hash) ?? 'session_expired');
        try { send(200, sessionAccess.setIdleEnabled(sessionAuthority.mintInputProof(hash, current.id, 'ui'), input.enabled)); }
        catch (error) { throw accessFailure(error); }
        return;
      }
      if (req.method === 'POST' && path === '/api/v1/auth/activity') {
        fields(await body(req, 1024), []);
        const current = store.authenticate(hash);
        if (!current || current.id !== user.id) throw new HttpError(401, store.sessionRevocationReason(hash) ?? 'session_expired');
        try { send(200, acceptHumanInput(hash, current.id, 'ui')); }
        catch (error) { throw accessFailure(error); }
        return;
      }
      if (path === '/api/v1/client/diagnostics') {
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        if (!clientDiagnosticAttempts.take(user.id, Date.now())) throw new HttpError(429, 'too_many_diagnostics');
        const input = await body(req, 16384);
        const current = store.authenticate(hash);
        if (!current || current.must_change_credentials) throw new HttpError(401, 'session_expired');
        const accepted = receiveClientDiagnostics(store, current, input);
        noteClientReport(store, hash, current.id, input);
        send(202, accepted, false); return;
      }
      if (req.method === 'POST' && path === '/api/v1/auth/password') {
        const input = await body(req);
        const currentSession = () => {
          const current = store.authenticate(hash);
          if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, store.sessionRevocationReason(hash, Date.now(), user.id) ?? 'session_expired');
        };
        try {
          fields(input, ['currentPassword', 'newPassword', 'passwordConfirmation']);
          if (typeof input.newPassword !== 'string' || input.newPassword !== input.passwordConfirmation) throw new HttpError(400, 'password_confirmation_mismatch');
          if (typeof input.currentPassword !== 'string' || !input.currentPassword || input.currentPassword.length > 256) throw new HttpError(400, 'invalid_current_password');
          if (input.newPassword.length < 10 || input.newPassword.length > 256) throw new HttpError(400, 'password_length_10_to_256');
          currentSession();
          if (!accounts.take('password:' + user.id, Date.now())) throw new HttpError(429, 'too_many_attempts');
          // A renamed/reused username cannot change the immutable request owner.
          const current = store.findUserById(user.id);
          if (!current) throw new HttpError(401, 'session_expired');
          const replacement = await passwordJob(async () => {
            if (!await verifyPassword(input.currentPassword, current.password_hash)) throw new HttpError(403, 'current_password_incorrect');
            return hashPassword(input.newPassword);
          });
          currentSession();
          try { store.changeOwnPassword(user.id, current.password_hash, replacement, hash); }
          catch (error) {
            if (error.message === 'session_expired') throw new HttpError(401, store.sessionRevocationReason(hash, Date.now(), user.id) ?? 'session_expired');
            if (error.message === 'credentials_changed') throw new HttpError(409, 'credentials_changed');
            throw error;
          }
        } finally { input.currentPassword = ''; input.newPassword = ''; input.passwordConfirmation = ''; }
        send(200, { ok: true, requiresLogin: true }); return;
      }
      if (req.method === 'GET' && path === '/api/v1/auth/me') { send(200, { user: publicUser(user), expiresAt: new Date(user.expires_at).toISOString() }); return; }
      if (path === '/api/v1/auth/renew') {
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        if (req.headers['transfer-encoding'] || Number(req.headers['content-length']) > 0) throw new HttpError(400, 'unexpected_body');
        const expiresAt = store.renewSession(hash, Date.now(), sessionTtlMs, sessionMaxLifetimeMs);
        if (expiresAt === null) throw new HttpError(401, 'session_expired');
        send(200, { expiresAt: new Date(expiresAt).toISOString(), maxLifetimeMs: sessionMaxLifetimeMs }); return;
      }
      // notice-v1 (notices.mjs): the notices this account's app shows now, and
      // the ones the person has seen. Reading never wakes the live channel and
      // is not activity (it does not move the idle sign-out).
      if (path === '/api/v1/notices') {
        if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
        send(200, notices.clientList(user, hash), false); return;
      }
      if (path === '/api/v1/notices/seen') {
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        const input = await body(req, 2048);
        const current = store.authenticate(hash);
        if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, store.sessionRevocationReason(hash, Date.now(), user.id) ?? 'session_expired');
        send(200, notices.markSeen(current, input), false); return;
      }
      // 지우기: the person removed these from their own list (per account, kept).
      if (path === '/api/v1/notices/remove') {
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        const input = await body(req, 2048);
        const current = store.authenticate(hash);
        if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, store.sessionRevocationReason(hash, Date.now(), user.id) ?? 'session_expired');
        send(200, notices.remove(current, input), false); return;
      }
      if (req.method === 'POST' && path === '/api/v1/auth/logout') { store.logout(hash); store.audit('logout', user.id, ''); send(200, { ok: true }); return; }
      const webviewMatch = /^\/api\/v1\/desktops\/([^/]+)\/(console-webview|console-webviews\/close)$/.exec(path);
      if (webviewMatch || path === '/api/v1/console-webviews/close') {
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        browserConsole.validate(req, requestOrigin);
        const input = await body(req, 256);
        const scopedClose = webviewMatch?.[2] === 'console-webviews/close';
        fields(input, scopedClose ? ['ownerId'] : []);
        if (Object.hasOwn(input, 'ownerId') && (typeof input.ownerId !== 'string' || !/^[a-f0-9]{64}$/.test(input.ownerId))) throw new HttpError(400, 'invalid_console_webview_owner');
        const current = store.authenticate(hash);
        if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, 'session_expired');
        if (webviewMatch) {
          try { desktopId(webviewMatch[1]); } catch { throw new HttpError(400, 'invalid_desktop_id'); }
        }
        if (webviewMatch?.[2] === 'console-webview') {
          relay.assertAccessAdmission?.();
          browserConsole.desktop({ user: current }, webviewMatch[1]);
          send(200, browserConsole.webviews.create(current, hash, webviewMatch[1], requestOrigin));
        } else {
          browserConsole.webviews.close(hash, requestOrigin, webviewMatch?.[1], input.ownerId);
          relay.revalidate();
          send(200, { closed: true });
        }
        return;
      }
      if (req.method === 'GET' && path === '/api/v1/desktops') {
        // features: what this broker adds to the list (a client that does not
        // find it, or finds a lower number, runs without that feature).
        send(200, { desktops: await desktopSnapshot(hash, { origin: requestOrigin, signinProtocol: req.headers['x-multitap-signin-protocol'] }), features: { desktopOrganize: 1 }, server: { version: VERSION } }); return;
      }
      const favoriteMatch = /^\/api\/v1\/desktops\/([^/]+)\/favorite$/.exec(path);
      if (favoriteMatch) {
        if (req.method !== 'PUT') throw new HttpError(405, 'method_not_allowed');
        let id; try { id = desktopId(decodeURIComponent(favoriteMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        // Same answer for an unknown and an unassigned VM: no existence leak.
        if (!store.assignedDesktop(user.id, id)) throw new HttpError(404, 'desktop_not_found');
        if (!favoriteAttempts.take(user.id, Date.now())) throw new HttpError(429, 'too_many_favorite_attempts');
        const input = await body(req, 256); fields(input, ['favorite']);
        if (typeof input.favorite !== 'boolean') throw new HttpError(400, 'invalid_favorite');
        const current = store.authenticate(hash);
        if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, 'session_expired');
        if (!store.setFavorite(current.id, id, input.favorite)) throw new HttpError(404, 'desktop_not_found');
        // send() wakes the live channel: every signed-in device of this account
        // (including the one that asked) gets desktops.changed and re-reads.
        send(200, { favorite: input.favorite }); return;
      }
      // desktop-preview-v1 (desktop-previews.mjs): the launcher picture of one
      // assigned VM. The Host is asked at most every 30 s; the picture stays in
      // memory only. Reading it never wakes the live channel.
      const previewMatch = /^\/api\/v1\/desktops\/([^/]+)\/preview$/.exec(path);
      if (previewMatch) {
        if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
        let id; try { id = desktopId(decodeURIComponent(previewMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        const assigned = () => {
          const current = store.authenticate(hash);
          if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, 'session_expired');
          const desktop = store.assignedDesktop(current.id, id);
          if (!desktop || (desktop.kind !== 'vm' && desktop.kind !== 'pc')) throw new HttpError(404, 'desktop_not_found');
        };
        assigned();
        if (!previewAttempts.take(user.id, Date.now())) throw new HttpError(429, 'too_many_preview_requests');
        if (!relay.previews.enabled()) throw new HttpError(409, 'desktop_previews_disabled');
        const host = relay.hosts.get(id);
        // Another account's session on this desktop: no new picture for this viewer while it lasts.
        const value = await relay.previews.view(id, host, relay.hostAuthenticated(host), relay.heartbeatMs, { othersSince: relay.othersSince(id, user.id) });
        // The account, its assignment and the switch are checked again after the wait.
        assigned();
        if (!relay.previews.enabled()) throw new HttpError(409, 'desktop_previews_disabled');
        send(200, value, false); return;
      }
      const cancelMatch = /^\/api\/v1\/sessions\/([0-9a-f-]{36})$/.exec(path);
      if (cancelMatch) {
        if (req.method !== 'DELETE') throw new HttpError(405, 'method_not_allowed');
        if (req.headers['transfer-encoding'] || Number(req.headers['content-length']) > 0) throw new HttpError(400, 'unexpected_body');
        send(200, relay.cancel(user, hash, cancelMatch[1])); return;
      }
      const consoleSessionMatch = /^\/api\/v1\/desktops\/([^/]+)\/console-sessions(?:\/([a-f0-9-]{36}))?$/.exec(path);
      if (consoleSessionMatch) {
        let id; try { id = desktopId(decodeURIComponent(consoleSessionMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        if (req.method === 'DELETE' && consoleSessionMatch[2]) {
          if (req.headers['transfer-encoding'] || Number(req.headers['content-length']) > 0) throw new HttpError(400, 'unexpected_body');
          send(200, relay.console.cancel(user, hash, id, consoleSessionMatch[2])); return;
        }
        if (req.method !== 'POST' || consoleSessionMatch[2]) throw new HttpError(405, 'method_not_allowed');
        const input = await body(req, 512); fields(input, []);
        const current = store.authenticate(hash);
        if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, 'session_expired');
        const abort = new AbortController();
        const abandoned = () => { if (!res.writableEnded) abort.abort(); };
        res.once('close', abandoned);
        try { send(201, { consoleSession: await relay.console.create(current, hash, id, { signal: abort.signal, origin: requestOrigin }) }); }
        finally { res.off('close', abandoned); }
        return;
      }
      const consoleMatch = /^\/api\/v1\/desktops\/([^/]+)\/console-preparation(?:\/operations\/([^/]+))?$/.exec(path);
      if (consoleMatch) {
        let id; try { id = desktopId(decodeURIComponent(consoleMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        if (!store.assignedDesktop(user.id, id)) throw new HttpError(404, 'desktop_not_found');
        if (req.method === 'GET') { send(200, relay.consolePreparation.status(user, id, consoleMatch[2])); return; }
        if (req.method === 'POST' && !consoleMatch[2]) {
          const input = await body(req); fields(input, ['confirmation']);
          if (!store.authenticate(hash)) throw new HttpError(401, 'session_expired');
          if (!displayAttempts.take(user.id, Date.now())) throw new HttpError(429, 'too_many_display_attempts');
          send(202, relay.consolePreparation.start(user, hash, id, input.confirmation)); return;
        }
        throw new HttpError(405, 'method_not_allowed');
      }
      const displayMatch = /^\/api\/v1\/desktops\/([^/]+)\/display(?:\/operations\/([^/]+))?$/.exec(path);
      if (displayMatch) {
        let id; try { id = desktopId(decodeURIComponent(displayMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        if (!store.assignedDesktop(user.id, id)) throw new HttpError(404, 'desktop_not_found');
        if (req.method === 'GET') { send(200, relay.displayStatus(user, id, displayMatch[2])); return; }
        if (req.method === 'POST' && !displayMatch[2]) {
          const input = await body(req); fields(input, ['count']);
          if (!store.authenticate(hash)) throw new HttpError(401, 'session_expired');
          if (!displayAttempts.take(user.id, Date.now())) throw new HttpError(429, 'too_many_display_attempts');
          send(202, relay.configureDisplay(user, hash, id, input.count)); return;
        }
        throw new HttpError(405, 'method_not_allowed');
      }
      const audioMatch = /^\/api\/v1\/desktops\/([^/]+)\/audio$/.exec(path);
      if (audioMatch) {
        let id; try { id = desktopId(decodeURIComponent(audioMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        const authorize = () => {
          const current = store.authenticate(hash);
          if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, 'session_expired');
          const desktop = store.assignedDesktop(current.id, id);
          if (!desktop) throw new HttpError(404, 'desktop_not_found');
          return { user: current, desktop };
        };
        authorize();
        if (!audioAttempts.take(user.id, Date.now())) throw new HttpError(429, 'too_many_vm_audio_attempts');
        if (req.method === 'GET') { send(200, { audio: await vmAudio.status(authorize) }); return; }
        if (req.method === 'POST') {
          const input = await body(req, 1024); fields(input, ['protocol', 'action', 'requestId', 'revision', 'confirm']);
          send(200, { audio: await vmAudio.add(authorize, input) }); return;
        }
        throw new HttpError(405, 'method_not_allowed');
      }
      const powerMatch = /^\/api\/v1\/desktops\/([^/]+)\/power(?:\/tasks\/([^/]+))?$/.exec(path);
      if (powerMatch) {
        let id; try { id = desktopId(decodeURIComponent(powerMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        const authorize = () => {
          if (!store.authenticate(hash)) throw new HttpError(401, 'session_expired');
          const desktop = store.assignedDesktop(user.id, id);
          if (!desktop) throw new HttpError(404, 'desktop_not_found');
          return desktop;
        };
        // The same route and assignment rule for an approved PC (pc-power.mjs).
        const control = authorize().kind === 'pc' ? pcPower : power;
        if (req.method === 'GET') { send(200, await control.status(authorize, user.id, powerMatch[2])); return; }
        if (req.method === 'POST' && !powerMatch[2]) {
          const input = await body(req); fields(input, ['action']);
          // A refusal before Proxmox (state, display change, Host update,
          // rate) is audited too: before, nothing showed why a power action
          // never reached the VM.
          const refused = error => { if (error instanceof HttpError && powerRefusals.has(error.code) && typeof input.action === 'string' && input.action.length <= 16) store.audit('power_refused', user.id, `${id}:${input.action}:${error.code}`); };
          if (!powerAttempts.take(user.id, Date.now())) { const error = new HttpError(429, 'too_many_power_attempts'); refused(error); throw error; }
          try { send(202, await control.submit(authorize, user.id, input.action)); } catch (error) { refused(error); throw error; }
          return;
        }
        throw new HttpError(405, 'method_not_allowed');
      }
      const takeoverContextMatch = /^\/api\/v1\/desktops\/([^/]+)\/takeover-context$/.exec(path);
      if (takeoverContextMatch) {
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        let id; try { id = desktopId(decodeURIComponent(takeoverContextMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        if (!requestedSessionContext(req, 'x-multitap-takeover-context')) throw new HttpError(400, 'invalid_context_protocol');
        const input = req.headers['transfer-encoding'] || Number(req.headers['content-length']) > 0 ? await body(req, 1024) : {};
        fields(input, []);
        send(200, relay.takeoverContext(user, hash, id)); return;
      }
      const takeoverMatch = /^\/api\/v1\/desktops\/([^/]+)\/takeover$/.exec(path);
      if (takeoverMatch) {
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        let id; try { id = desktopId(decodeURIComponent(takeoverMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        const input = req.headers['transfer-encoding'] || Number(req.headers['content-length']) > 0 ? await body(req) : {};
        const contextual = requestedSessionContext(req, 'x-multitap-takeover-context');
        fields(input, contextual ? ['context'] : []);
        if (contextual && (typeof input.context !== 'string' || !/^[A-Za-z0-9_-]{43}$/.test(input.context))) throw new HttpError(400, 'invalid_takeover_context');
        if (!takeoverAttempts.take(user.id, Date.now())) throw new HttpError(429, 'too_many_takeover_attempts');
        send(200, relay.takeover(user, hash, id, contextual ? { context: input.context } : undefined)); return;
      }
      // 'Console 끊고 연결': an assigned account ends the VM's
      // console connections (anyone's, like stream takeover) so its stream can
      // start; the console's QEMU-display reservation goes with them.
      const consoleReleaseMatch = /^\/api\/v1\/desktops\/([^/]+)\/console\/release$/.exec(path);
      if (consoleReleaseMatch) {
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        let id; try { id = desktopId(decodeURIComponent(consoleReleaseMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        fields(req.headers['transfer-encoding'] || Number(req.headers['content-length']) > 0 ? await body(req) : {}, []);
        if (!store.assignedDesktop(user.id, id)) throw new HttpError(404, 'desktop_not_found');
        if (!takeoverAttempts.take(user.id, Date.now())) throw new HttpError(429, 'too_many_takeover_attempts');
        send(200, { released: relay.console.release(user, id) }); return;
      }
      const hostUpdateMatch = /^\/api\/v1\/desktops\/([^/]+)\/host-update$/.exec(path);
      if (hostUpdateMatch) {
        // Remote Host program update from the Windows client by any account the
        // VM is assigned to (it could install it by connecting anyway). Only to
        // the catalog's latest Host and never older (enforced by hostUpdate for
        // source 'client'); disconnecting now ends only the requester's own
        // connection unless the requester is an administrator.
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        let id; try { id = desktopId(decodeURIComponent(hostUpdateMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        if (!store.assignedDesktop(user.id, id)) throw new HttpError(404, 'desktop_not_found');
        const input = await body(req, 1024); fields(input, ['version', 'disconnectSessions']);
        if (!hostUpdateAttempts.take(user.id, Date.now())) throw new HttpError(429, 'too_many_host_update_attempts');
        const current = store.authenticate(hash);
        if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, 'session_expired');
        if (!store.assignedDesktop(current.id, id)) throw new HttpError(404, 'desktop_not_found');
        if (input.disconnectSessions === true && current.role !== 'admin' && [...relay.sessions.values()].some(session => session.desktopId === id && session.userId !== current.id)) throw new HttpError(409, 'desktop_busy', 'desktop_busy', { reason: 'other_user', takeover: false });
        const authorize = () => {
          const viewer = store.authenticate(hash);
          if (!viewer || viewer.id !== current.id || viewer.must_change_credentials) throw new HttpError(401, 'session_expired');
          if (!store.assignedDesktop(viewer.id, id)) throw new HttpError(404, 'desktop_not_found');
          return { ownSessionsOnly: viewer.role === 'admin' ? undefined : viewer.id };
        };
        send(202, { hostUpdate: await relay.hostUpdate.request({ actor: current.id, desktopId: id, version: input.version, disconnectSessions: input.disconnectSessions ?? false, source: 'client', ownSessionsOnly: current.role === 'admin' ? undefined : current.id, authorize }) }); return;
      }
      // host-ready-wait-v1 (host-ready-wait.mjs): held until the desktop's
      // Host has reported on this broker, at most the relay's hold time.
      const hostReadyMatch = /^\/api\/v1\/desktops\/([^/]+)\/host-ready$/.exec(path);
      if (hostReadyMatch) {
        if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
        let id; try { id = desktopId(decodeURIComponent(hostReadyMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        // Same answer for an unknown and an unassigned desktop, as the list.
        if (!store.listDesktops(user.id).some(item => item.id === id)) throw new HttpError(404, 'desktop_not_found');
        const ready = await relay.hostReadyWaits.wait(id, user.id, res);
        if (res.destroyed) return;
        // The wait can outlast this sign-in or the assignment.
        const current = store.authenticate(hash);
        if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, 'session_expired');
        if (!store.listDesktops(current.id).some(item => item.id === id)) throw new HttpError(404, 'desktop_not_found');
        send(200, { protocol: hostReadyProtocol, ready }, false); return;
      }
      const desktopMatch = /^\/api\/v1\/desktops\/([^/]+)(\/connect)?$/.exec(path);
      if (desktopMatch && (req.method === 'GET' || req.method === 'POST')) {
        let id; try { id = desktopId(decodeURIComponent(desktopMatch[1])); } catch { throw new HttpError(404, 'desktop_not_found'); }
        const desktop = store.listDesktops(user.id).find(item => item.id === id);
        if (!desktop) throw new HttpError(404, 'desktop_not_found');
        if (req.method === 'GET' && !desktopMatch[2]) { await power.refresh(); const viewer = store.authenticate(hash); if (!viewer) throw new HttpError(401, 'session_expired'); const current = store.listDesktops(user.id).find(value => value.id === id); if (!current) throw new HttpError(404, 'desktop_not_found'); const views = await withHostUpdates(vmStates.decorate(power.decorate(relay.decorate([current], user.id, requestOrigin), user.id)), viewer); const available = req.headers['x-multitap-signin-protocol'] === 'signin-stream-v1' ? relay.withSigninAvailability(views, hash, user.id) : views; if (!available[0]) throw new HttpError(404, 'desktop_not_found'); send(200, { desktop: available[0] }); return; }
        if (req.method === 'POST' && desktopMatch[2]) {
          // Older stream clients send an empty POST. New clients select a
          // connection mode only; no destination, port or Windows credentials.
          const input = req.headers['transfer-encoding'] || Number(req.headers['content-length']) > 0 ? await body(req) : {};
          const current = store.authenticate(hash);
          if (!current || current.id !== user.id || current.must_change_credentials) throw new HttpError(401, store.sessionRevocationReason(hash, Date.now(), user.id) ?? 'session_expired');
          // `rdp` comes with mode 'rdp' from a released client: it is accepted so
          // that the request reaches its refusal (rdp_not_supported), and not read.
          fields(input, ['rdp', 'signinProtocol', 'signinAvailabilityId', 'mode', 'options', 'resizeProtocol', 'clipboardProtocol', 'mediaProtocol', 'tracks', 'streamProfileTracksProtocol', 'initialViewportProtocol', 'initialViewport', 'viewportEncoderLimitsProtocol', 'inputLatencyProtocol', 'observabilityProtocol', 'cursorProtocol', 'cursorTracksProtocol', 'textInputProtocol', 'displayScaleProtocol', 'displayScale', 'displayScaleOutputsProtocol', 'touchInputProtocol', 'relativeMouseProtocol', 'caretProtocol', 'cursorEchoProtocol', 'cursorComposeProtocol', 'usbProtocol', 'streamSizeProtocol', 'decoderLimits', 'printProtocol', 'driveProtocol', 'driveFs', 'udpMediaProtocol', 'udpDirectProtocol', 'udpTracksProtocol', 'sessionResumeProtocol', 'audioCodecProtocol']);
          const granted = relay.connect(user, hash, id, input.mode === undefined ? 'stream' : input.mode, input.options, input.resizeProtocol, input.clipboardProtocol, {signinProtocol:input.signinProtocol,signinAvailabilityId:input.signinAvailabilityId,mediaProtocol:input.mediaProtocol,tracks:input.tracks,streamProfileTracksProtocol:input.streamProfileTracksProtocol,initialViewportProtocol:input.initialViewportProtocol,initialViewport:input.initialViewport,viewportEncoderLimitsProtocol:input.viewportEncoderLimitsProtocol,inputLatencyProtocol:input.inputLatencyProtocol,observabilityProtocol:input.observabilityProtocol,cursorProtocol:input.cursorProtocol,cursorTracksProtocol:input.cursorTracksProtocol,textInputProtocol:input.textInputProtocol,displayScaleProtocol:input.displayScaleProtocol,displayScale:input.displayScale,displayScaleOutputsProtocol:input.displayScaleOutputsProtocol,touchInputProtocol:input.touchInputProtocol,relativeMouseProtocol:input.relativeMouseProtocol,caretProtocol:input.caretProtocol,cursorEchoProtocol:input.cursorEchoProtocol,cursorComposeProtocol:input.cursorComposeProtocol,usbProtocol:input.usbProtocol,streamSizeProtocol:input.streamSizeProtocol,decoderLimits:input.decoderLimits,printProtocol:input.printProtocol,driveProtocol:input.driveProtocol,driveFs:input.driveFs,udpMediaProtocol:input.udpMediaProtocol,udpDirectProtocol:input.udpDirectProtocol,udpTracksProtocol:input.udpTracksProtocol,sessionResumeProtocol:input.sessionResumeProtocol,audioCodecProtocol:input.audioCodecProtocol}, requestOrigin, req.brokerTls, requestedSessionContext(req, 'x-multitap-takeover-context'));
          usage.requested(granted.sessionId, address);
          send(200, granted); return;
        }
      }
      throw new HttpError(404, 'not_found');
    } catch (error) {
      if (res.destroyed || res.headersSent) return;
      const known = error instanceof HttpError;
      if (!known) logger.error(JSON.stringify({ event: 'request_error', requestId, type: error.name }));
      // A person who typed or followed an address in a browser gets a card that says what to do, not the API's
      // JSON: a wrong address, HTTP turned off, a page that does not exist. Same status; every caller that does
      // not ask for a page first (apps, scripts, the console's own requests) keeps the JSON below.
      const path = (req.url ?? '').split('?')[0];
      const page = known && ['GET', 'HEAD'].includes(req.method) && ['origin_not_allowed', 'http_disabled', 'not_found'].includes(error.code) && prefersHtml(req.headers.accept) &&
        !/^\/(?:api|internal|relay|downloads|console\/(?:api|assets|ws))(?:\/|$)/.test(path) ? refusalPage(refusalKind ?? error.code, requestLanguage(req.headers['accept-language'])) : null;
      if (page) {
        res.writeHead(error.status, { 'Content-Type': 'text/html; charset=utf-8', 'Content-Length': page.length, 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff', 'Referrer-Policy': 'no-referrer', 'Content-Security-Policy': consoleHttpsPagePolicy, Vary: 'Accept, Accept-Language', ...securityHeaders, 'X-Request-Id': requestId });
        res.end(req.method === 'HEAD' ? undefined : page); return;
      }
      send(known ? error.status : 500, { error: { code: known ? error.code : 'internal_error', message: known ? error.message : 'Request failed.', ...(known && error.details ? { details: error.details } : {}) } });
    }
  };
  const additional = new Set();
  const attachListener = (listener, ownsLifecycle) => {
    listener.headersTimeout = 10000;
    listener.requestTimeout = 15000;
    listener.setTimeout(20000);
    listener.maxHeadersCount = 60;
    relay.attach(listener, { ownsLifecycle, upgrade: (req, socket, head, url, reject) => {
      req.brokerOrigin = publicOrigin;
      req.brokerTls = observabilityTls(req, { origin: publicOrigin });
      if (strictAccess) {
        try {
          const context = resolveRequestAccess(req, accessConfig, { bootstrap, setupOrigin });
          if (context.bootstrapOnly) { reject(403, 'Forbidden'); return true; }
          req.brokerOrigin = context.origin;
          req.brokerTls = observabilityTls(req, context);
        } catch { reject(403, 'Forbidden'); return true; }
      }
      return browserConsole.upgrade(req, socket, head, url, reject, req.brokerOrigin) || desktopEvents.upgrade(req, socket, head, url, reject);
    } });
    return listener;
  };
  const server = attachListener(tls === null ? createServer(requestListener) : createTlsServer(tls, requestListener), true);
  server.createAdditionalListener = (tlsOptions = null) => {
    const listener = attachListener(tlsOptions === null ? createServer(requestListener) : createTlsServer(tlsOptions, requestListener), false);
    additional.add(listener); listener.once('close', () => additional.delete(listener));
    return listener;
  };
  const close = server.close.bind(server);
  server.close = callback => {
    clearInterval(idleSweep);
    usage.close(); backup.close(); accessRuntime.close(); vmStates.close(); catalogWatch.close(); desktopEvents.close(); relay.hostUpdate.close(); pcPower.close(); retention.close(); certificateMonitor?.close();
    const others = [...additional]; let waiting = others.length + 1;
    // A connection whose request was still running at close() stays open after
    // its answer for the keep-alive time (Node: up to 5 s), and the process
    // exits only after it: an update's downtime grows by that much. Each one is
    // closed as soon as its answer is out (restart recovery).
    const listeners = [server, ...others];
    const sweep = setInterval(() => { for (const listener of listeners) listener.closeIdleConnections?.(); }, 50); sweep.unref();
    const done = () => { if (--waiting === 0) { clearInterval(sweep); if (store.sessionAccessGuard === accessGuard) delete store.sessionAccessGuard; callback?.(); } };
    for (const listener of others) listener.close(done);
    close(done); return server;
  };
  server.accessRuntime = accessRuntime;
  server.backup = backup; // tests drive the step after a restore through it
  server.relay = relay; // tests and the administration console read live sessions through it
  server.sessionAccess = sessionAccess; // owner-bound policy; no peer proof mint is exposed
  return server;
}
