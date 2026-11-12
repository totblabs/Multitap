// Multitap administrator console: state, API access, routing, the console
// frame (navigation, breadcrumbs, search) and shared dialogs. Pages live in
// ./js/*.js and receive this module's context object; they keep no state of
// their own, so each console instance (and each DOM test) is independent.
import { commandBox } from './proxmox-shell-setup.js';
import { el, $, button, icon, stateDot } from './js/dom.js';
import { messages, updateActive, hostUpdateActive } from './js/labels.js';
import * as overview from './js/overview.js';
import * as desktops from './js/desktops.js';
import * as users from './js/users.js';
import * as hosts from './js/hosts.js';
import * as monitor from './js/monitor.js';
import * as usage from './js/usage.js';
import * as system from './js/system.js';
import * as serverAccess from './js/server-access.js';
import * as downloadsPage from './js/downloads.js';
import * as backupPage from './js/backup.js';
import * as noticesPage from './js/notices.js';
import { createServerUpdateBadge } from './js/server-update-badge.js';
import { normalizeMfaCode, normalizeMfaRecoveryCode, createMfaDeadline, requestMfaAuthentication, retireMfaCookie, MfaClientError } from './js/mfa-auth.js';
import { createAccountSecurityController } from './js/account-security-controller.js';
import { createAccountSecurityView } from './js/account-security-view.js';
import { t, lang, translatePage, wireLanguageSwitch } from './js/i18n.js';

const ownDocument = globalThis.document;
const serverUpdateBadge = createServerUpdateBadge();
// The static text of index.html in the chosen language (Korean is already there), and the language switch.
translatePage(document); wireLanguageSwitch(document);
// Static placeholders in index.html (navigation, top bar, dialog) receive their icons here.
for (const node of document.querySelectorAll('[data-icon]')) node.replaceChildren(icon(node.getAttribute('data-icon')));

const state = { user: null, csrf: '', route: { page: 'overview', id: null }, data: null, loading: false, pveBusy: false, pveError: '', pveLoaded: false, onboarding: null,
  serverInfo: null, serverInfoLoading: false, serverInfoError: '', serverUpdatePending: false, serverUpdateSubmitting: false, serverRestart: null,
  resources: new Map(), ui: {}, expanded: new Map(), assignmentSaving: false };
let modalRevision = 0, serverInfoRevision = 0, serverInfoAt = 0, serverApplyAt = 0, serverUpdateTimer = null, hostUpdateTimer = null, pollTimer = null, serverLost = false;
let modalDraftDirty = () => false;
// One line for the sign-in page, set by what is about to end this sign-in on
// purpose (a restore: the accounts become the backup's).
let loginNotice = '';
let activeModalSubmission = null, modalBusy = false;
// A new login is a new owner even when its account object/id is reused. This
// fences local async UI effects; it does not cancel or replay server writes.
let sessionGeneration = 0, inventoryRevision = 0, refreshRevision = 0;
let accountSecurityController = null, authRevision = 0, adminMfaChallenge = null, adminMfaBusy = false, adminMfaRecovery = false, adminMfaTimer = null;
const sessionOwner = () => ({ generation: sessionGeneration, user: state.user, csrf: state.csrf, origin: globalThis.location.origin });
const sessionCurrent = owner => globalThis.document === ownDocument && owner.generation === sessionGeneration && owner.user === state.user && owner.csrf === state.csrf && owner.origin === globalThis.location.origin;
const sessionChanged = () => Object.assign(new Error(t('app.1')), { code: 'admin_session_changed' });
const pageActions = new WeakMap();
function pageAction(key) {
  const owner = sessionOwner(), ui = state.ui, token = {};
  let actions = pageActions.get(ui);
  if (!actions) { actions = new Map(); pageActions.set(ui, actions); }
  actions.set(key, token);
  const current = () => sessionCurrent(owner) && state.ui === ui && actions.get(key) === token;
  return { current, check: () => { if (!current()) throw sessionChanged(); } };
}

// Pages: title, the navigation entry they belong to, the resources they show,
// and whether opening them synchronises the Proxmox inventory.
// Every page is entered from the sidebar (nothing may hide one
// level down). Three groups: 홈 · 데스크톱 · 사용자 · 기록 · 모니터링 · 공지, then
// 업데이트 · 다운로드, then the group headed 설정 with one entry per settings
// page (계정 보안 is its dialog). There is no 설정 overview page. A page with a
// parent is one item of that list: it shows the list with the item's window
// (창, dom.js detailWindow) open over it and keeps the list's entry selected;
// `group` names the sidebar heading a page stands under (the first crumb).
const pageDefs = {
  overview: { title: t('common.1'), render: overview.render, needs: () => ['sessions', 'hosts', 'clientDiagnostics', 'hostRequests', 'resources', ...(state.data && !state.data.assignments?.length ? ['serverAccess'] : [])] },
  log: { title: t('common.2'), render: monitor.log, needs: route => monitor.logNeeds(route) },
  monitoring: { title: t('common.3'), render: usage.render, needs: route => usage.usageNeeds(ctx, route) },
  // 공지 (js/notices.js). The pages whose operations end every connection also read `notices`: 먼저 알리기
  // needs to know whether anybody is signed in (never during the first setup, which has no such read).
  notices: { title: t('common.4'), render: noticesPage.render, needs: () => ['notices'] },
  desktops: { title: t('common.5'), render: desktops.list, needs: route => desktops.workspaceNeeds(ctx, route), sync: true },
  desktop: { title: route => { const d = hosts.registeredHostIds(ctx)?.has(route.id) ? desktopById(route.id) : null; return d ? d.kind === 'pc' ? t('common.6', { name: d.name }) : `VM ${d.pve_vmid} · ${d.name}` : t('common.5'); }, parent: 'desktops', render: desktops.detail, needs: route => desktops.workspaceNeeds(ctx, route), sync: true },
  users: { title: t('common.7'), render: users.list, needs: () => ['sessions'] },
  user: { title: route => route.id ?? t('common.7'), parent: 'users', render: users.detail, needs: () => ['clientDiagnostics', 'sessions', 'hosts'], sync: true },
  updates: { title: t('app.updatesPage'), render: system.updates, needs: () => ['hosts', 'sessions', 'notices'] },
  downloads: { title: t('html.64'), render: downloadsPage.render, needs: () => ['downloads'] },
  // During the first setup its step list says whether the server already has an address (js/system.js).
  proxmox: { title: t('common.10'), group: t('common.11'), render: system.proxmox, needs: () => setupRequired() ? ['serverAccess'] : [], sync: true },
  'server-access': { title: t('common.12'), group: t('common.11'), render: serverAccess.render, needs: () => setupRequired() ? ['serverAccess'] : ['serverAccess', 'notices'] },
  backup: { title: t('common.13'), group: t('common.11'), render: backupPage.render, needs: () => setupRequired() ? ['backup'] : ['backup', 'notices'] },
  'server-info': { title: t('common.14'), group: t('common.11'), render: system.serverInformation, needs: () => [] },
  licenses: { title: t('common.15'), group: t('common.11'), render: system.licenses, needs: () => [] },
};
const navOf = page => pageDefs[page].parent ?? page;
// Until Proxmox is connected only these are offered: connecting it, the server's
// address, and putting a backup back (a new server may start from one).
const firstSetupPages = ['proxmox', 'server-access', 'backup'];
const pageTitle = route => { const title = pageDefs[route.page].title; return typeof title === 'function' ? title(route) : title; };

// Resources: read-only data loaded on demand, cached briefly and refreshed
// while their page is open. A value is replaced only by a valid response.
const resourceDefs = {
  serverAccess: { path: () => '/server-access', valid: serverAccess.validResponse, maxAge: 4000 },
  backup: { path: () => '/backup', valid: backupPage.validResponse, maxAge: 4000 },
  sessions: { path: () => '/sessions', valid: v => Array.isArray(v?.sessions) },
  hosts: { path: () => '/hosts', valid: v => Array.isArray(v?.hosts) },
  clientDiagnostics: { path: () => '/client-diagnostics', valid: v => Array.isArray(v?.events) },
  clientDiagnosticsVm: { path: id => '/desktops/' + encodeURIComponent(id) + '/client-diagnostics', valid: v => Array.isArray(v?.events) },
  streamStats: { path: () => '/stream-stats', valid: v => Array.isArray(v?.sessions) },
  // 모니터링 (js/usage.js): the id is the rest of the path, e.g. usageSummary:2026-11-04/2026-11-10.
  usageLive: { path: () => '/usage/live', valid: v => Array.isArray(v?.logins) && Array.isArray(v?.connections) && !!v.today && !!v.totals },
  usageSummary: { path: id => '/usage/summary/' + id, valid: v => Array.isArray(v?.users) && Array.isArray(v?.desktops) && Array.isArray(v?.days), maxAge: 20000 },
  usageEvents: { path: id => '/usage/events/' + id, valid: v => Array.isArray(v?.events) && Array.isArray(v?.accounts), maxAge: 20000 },
  // 공지 (js/notices.js): who is signed in now and the sent notices. No clock in the answer.
  notices: { path: () => '/notices', valid: v => Array.isArray(v?.notices) && Array.isArray(v?.audience?.logins) && Array.isArray(v?.audience?.connections), maxAge: 4000 },
  // 홈 자원 (js/resources.js): the server process and the running desktops.
  resources: { path: () => '/resources', valid: v => v?.protocol === 'admin-resources-v1' && !!v.server && Array.isArray(v.desktops) && Array.isArray(v.nodes) },
  hostRequests: { path: () => '/hosts/requests', valid: v => Array.isArray(v?.requests), maxAge: 4000 },
  diagnostics: { path: id => '/desktops/' + encodeURIComponent(id) + '/diagnostics', valid: v => typeof v?.supported === 'boolean' },
  downloads: { public: true, valid: v => Array.isArray(v?.releases), maxAge: 60000 },
};
const resourceDef = name => { const index = name.indexOf(':'), kind = index < 0 ? name : name.slice(0, index), id = index < 0 ? undefined : name.slice(index + 1); return { kind: kind === 'clientDiagnostics' && id ? 'clientDiagnosticsVm' : kind, id }; };

// Toast (#notice): a success clears itself after a few seconds unless the
// pointer or focus is on it; an error stays until it is closed.
// A notice with an action ({ label, run }) also stays until it is closed.
let noticeTimer = null;
function notice(text, error = false, action = null) {
  const node = $('#notice'); placeNotice(); node.hidden = !text; node.className = error ? 'is-error' : 'is-success';
  node.replaceChildren(...(text ? [icon(error ? 'alert' : 'check'), el('span', { class: 'notice-text' }, text), ...(action ? [el('button', { type: 'button', class: 'notice-action', onclick: action.run }, action.label)] : []), el('button', { type: 'button', class: 'icon-button notice-close', 'aria-label': t('app.2'), onclick: () => notice('') }, icon('x'))] : []));
  clearTimeout(noticeTimer); noticeTimer = null;
  if (text && !error && !action) {
    const hide = () => {
      noticeTimer = null;
      if (globalThis.document !== ownDocument || node.textContent !== text + '') return;
      let held = false; try { held = node.matches(':hover') || node.contains(globalThis.document.activeElement); } catch { held = false; }
      if (held) { noticeTimer = setTimeout(hide, 2000); noticeTimer.unref?.(); return; }
      node.hidden = true; node.replaceChildren();
    };
    noticeTimer = setTimeout(hide, 6000); noticeTimer.unref?.();
  }
}
const adminReadTimeoutMs = 15000;
function readTimeoutError() {
  const error = new Error(t('app.3'));
  error.code = 'admin_read_timeout';
  return error;
}
async function readWithDeadline(read) {
  const controller = new AbortController();
  let timer;
  const deadline = new Promise((_, reject) => {
    timer = setTimeout(() => {
      controller.abort();
      reject(readTimeoutError());
    }, adminReadTimeoutMs);
    timer.unref?.();
  });
  try { return await Promise.race([read(controller.signal), deadline]); }
  finally { clearTimeout(timer); }
}
async function api(path, method = 'GET', data, { accept } = {}) {
  const owner = sessionOwner();
  const request = async signal => {
    const current = () => {
      if (!sessionCurrent(owner)) throw sessionChanged();
      if (signal?.aborted) throw readTimeoutError();
    };
    let response;
    try { response = await fetch('/api/v1/admin' + path, { method, credentials: 'same-origin', headers: { 'Accept-Language': lang, ...(data !== undefined ? { 'Content-Type': 'application/json' } : {}), ...(state.csrf ? { 'X-CSRF-Token': state.csrf } : {}) }, body: data === undefined ? undefined : JSON.stringify(data), ...(signal ? { signal } : {}) }); }
    catch { current(); serverLost = true; throw new Error(t('app.4')); }
    current();
    // A restarting server (or its proxy) answers without JSON: recheck its version once it is back.
    let body; try { body = await response.json(); } catch { current(); serverLost = true; throw new Error(t('app.5')); }
    current();
    if (!response.ok) {
      if (response.status === 401 && path !== '/login') showLogin();
      // A page that needs Proxmox goes to the Proxmox step; the other first-setup pages stay where they are.
      if (sessionCurrent(owner) && body.error?.code === 'proxmox_setup_required') { state.onboarding = { proxmoxRequired: true, complete: false }; if (!firstSetupPages.includes(state.route.page)) state.route = { page: 'proxmox', id: null }; if (state.data) render(); }
      const error = new Error((typeof body.error?.code === 'string' && Object.hasOwn(messages, body.error.code) ? messages[body.error.code] : null) || t('app.6')); error.code = body.error?.code; error.status = response.status; throw error;
    }
    return accept ? accept({ status: response.status, body }) : body;
  };
  try { return method === 'GET' ? await readWithDeadline(request) : await request(); }
  catch (error) {
    if (error?.code === 'admin_read_timeout') {
      if (!sessionCurrent(owner)) throw sessionChanged();
      serverLost = true;
    }
    throw error;
  }
}
async function productRelease(product) {
  try {
    const response = await fetch('/api/v1/downloads', { credentials: 'omit' });
    if (!response.ok) return null;
    const { releases } = await response.json();
    if (!Array.isArray(releases)) return null;
    return releases.filter(entry => entry.product === product && /^\/downloads\/Multitap(?:-Host)?-[0-9]+\.[0-9]+\.[0-9]+-windows-x64\.(?:zip|exe)$/.test(entry.url))
      .sort((a, b) => b.version.localeCompare(a.version, 'en', { numeric: true }) || Number(b.url.endsWith('.exe')) - Number(a.url.endsWith('.exe')))[0] || null;
  } catch { return null; }
}

function resource(name) { return state.resources.get(name) ?? null; }
const value = name => resource(name)?.value ?? null;
// What a resource says, to tell whether it changed. The moment of the answer (checkedAt) is not part of it:
// /sessions, /hosts and /usage/live carry a new one in every answer, so every poll would redraw the page
// (a text selection cleared and a chart's readout gone back every 5 s on pages where nothing has changed).
const resourceShape = entry => JSON.stringify(entry.value, (key, value) => key === 'checkedAt' ? undefined : value) + entry.error;
let resourceDrawAt = 0;
async function load(name, { force = false, maxAge } = {}) {
  if (!state.user) return;
  const { kind, id } = resourceDef(name), def = resourceDefs[kind];
  if (!def) return;
  const entry = state.resources.get(name) ?? { value: null, error: '', loading: false, at: 0 };
  state.resources.set(name, entry);
  if (entry.loading || (!force && entry.at && Date.now() - entry.at < (maxAge ?? def.maxAge ?? 9000))) return;
  const owner = sessionOwner(); entry.loading = true;
  const before = resourceShape(entry);
  try {
    let result;
    if (def.public) { const response = await fetch('/api/v1/downloads', { credentials: 'omit' }); result = response.ok ? await response.json() : null; }
    else result = await api(def.path(id));
    if (!sessionCurrent(owner)) return;
    if (!def.valid(result)) throw new Error(t('app.7'));
    entry.value = result; entry.error = '';
  } catch (error) { if (!sessionCurrent(owner)) return; entry.error = error.message || t('app.8'); }
  finally { if (sessionCurrent(owner) && state.resources.get(name) === entry) { entry.loading = false; entry.at = Date.now(); } }
  // Redrawn when what the answer says changed; with nothing changed, once a minute, so "3분 전" keeps moving.
  const changed = resourceShape(entry) !== before;
  if (sessionCurrent(owner) && state.data && (changed || Date.now() - resourceDrawAt >= 60000) && (currentNeeds().includes(name) || name === 'hostRequests')) {
    resourceDrawAt = Date.now();
    render();
    if (changed && name === 'hosts' && ['desktop', 'desktops'].includes(state.route.page)) loadPage();
  }
  if (name === 'hosts' && sessionCurrent(owner)) void followHosts();
}
// The inventory is read once and the Host list again every few seconds. Where the list says what the inventory
// does not (a Host that connected after its approval or went away, its version after an update, a desktop that
// was registered elsewhere), the inventory is read again, quietly: not during another read, not under an open
// form, and once per difference (otherwise an approved desktop stays 응답 없음 until 새로고침).
let inventoryDriftSeen = '', inventoryRereading = false;
function inventoryDrift() {
  const rows = value('hosts')?.hosts, desktops = state.data?.desktops;
  if (resource('hosts')?.error || !Array.isArray(rows) || !Array.isArray(desktops)) return '';
  const known = new Map(desktops.map(d => [d.id, d])), parts = [];
  for (const row of rows) {
    if (typeof row?.desktopId !== 'string' || typeof row.connected !== 'boolean') continue;
    const d = known.get(row.desktopId);
    if (!d) parts.push('+' + row.desktopId);
    else if (row.connected !== (d.host?.connected === true)) parts.push(row.desktopId + (row.connected ? ':on' : ':off'));
    else if (row.connected && typeof row.version === 'string' && typeof d.host?.version === 'string' && row.version !== d.host.version) parts.push(row.desktopId + ':' + row.version);
  }
  return parts.join(',');
}
async function followHosts() {
  const drift = inventoryDrift();
  if (!drift) { inventoryDriftSeen = ''; return; }
  if (drift === inventoryDriftSeen || inventoryRereading || state.loading || state.pveBusy || !state.data || setupRequired() || $('#modal').open) return;
  const owner = sessionOwner(), ticket = inventoryRevision;
  inventoryRereading = true; inventoryDriftSeen = drift;
  try {
    const inventory = await api('/inventory');
    if (!sessionCurrent(owner) || ticket !== inventoryRevision || state.loading || state.pveBusy || !state.data) return;
    // What the Proxmox check added to the connection's state stays.
    const unavailable = state.data.proxmox?.unavailableCount;
    state.data = inventory; state.onboarding = inventory.onboarding || state.onboarding;
    if (state.data.proxmox && state.data.proxmox.unavailableCount === undefined && unavailable !== undefined) state.data.proxmox.unavailableCount = unavailable;
    render();
  } catch { if (sessionCurrent(owner)) inventoryDriftSeen = ''; /* the next read of the Host list tries again */ }
  finally { inventoryRereading = false; }
}
const currentNeeds = () => pageDefs[state.route.page].needs(state.route);
const loadPage = (force = false) => { for (const name of currentNeeds()) void load(name, { force }); };

// Periodic refresh of the open page's resources; stops with the session or
// when this console's document is replaced.
function startPolling() {
  clearInterval(pollTimer);
  pollTimer = setInterval(() => {
    if (globalThis.document !== ownDocument) { clearInterval(pollTimer); return; }
    if (!state.user || !state.data || (setupRequired() && !['server-access', 'backup'].includes(state.route.page))) return;
    loadPage();
    // The 홈 badge counts 지금 할 일 on every page. Not during first setup:
    // those reads need Proxmox, and their refusal would pull 백업·복원 or
    // 서버 접속 설정 back to the Proxmox step.
    if (!setupRequired()) for (const name of ['hosts', 'hostRequests']) void load(name);
    if (newVersion) followServerVersion(newVersion);
    if (serverLost || Date.now() - serverInfoAt >= 600000) void loadServerInfo();
  }, 5000);
  pollTimer.unref?.();
}

function resetAdminMfa() {
  authRevision++; clearTimeout(adminMfaTimer); adminMfaTimer = null; if (adminMfaChallenge) adminMfaChallenge.challengeToken = ''; adminMfaChallenge = null; adminMfaBusy = false; adminMfaRecovery = false;
  const form = $('#admin-mfa-form'); form?.reset();
  if (form) for (const control of form.querySelectorAll('input,button')) control.disabled = false;
  if ($('#admin-mfa')) $('#admin-mfa').hidden = true;
  if ($('#admin-mfa-code')) { $('#admin-mfa-code').hidden = false; $('#admin-mfa-form').elements.code.disabled = false; }
  if ($('#admin-mfa-recovery')) { $('#admin-mfa-recovery').hidden = true; $('#admin-mfa-form').elements.recoveryCode.disabled = true; }
  if ($('#admin-mfa-mode')) $('#admin-mfa-mode').textContent = t('app.9');
  if ($('#admin-mfa-error')) $('#admin-mfa-error').textContent = '';
}
function showLogin({ completedMfa } = {}) {
  accountSecurityController?.retire({ preserveResult: completedMfa });
  resetAdminMfa();
  sessionGeneration++; inventoryRevision++; refreshRevision++;
  state.loading = false; $('#refresh').disabled = false; serverLost = false;
  const submit = $('#login-form').querySelector('button'); submit.disabled = false; submit.textContent = t('common.16');
  for (const button of $('#setup-form').querySelectorAll('button')) button.disabled = false;
  serverUpdateBadge.clear(); serverInfoAt = 0;
  clearTimeout(serverUpdateTimer); serverUpdateTimer = null; state.serverUpdatePending = false; state.serverUpdateSubmitting = false; state.serverRestart = null;
  clearTimeout(hostUpdateTimer); hostUpdateTimer = null; clearInterval(pollTimer); pollTimer = null;
  modalRevision++;
  activeModalSubmission = null;
  state.user = null; state.csrf = ''; state.data = null; state.resources.clear(); state.ui = {}; state.expanded.clear();
  state.pveBusy = false; state.pveError = ''; state.pveErrorCode = null; state.pveLoaded = false;
  state.onboarding = null; state.assignmentSaving = false; inventoryDriftSeen = ''; serverApplyAt = 0;
  serverInfoRevision++; state.serverInfo = null; state.serverInfoLoading = false; state.serverInfoError = ''; $('#broker-version').textContent = '—'; $('#server-version-header').textContent = t('common.17');
  shutDetail();
  $('#console').hidden = true; $('#setup').hidden = true; $('#login').hidden = false; renderExpiryBanner();
  showSetupRestore(false);
  const announce = $('#login-notice'); if (announce) { announce.textContent = loginNotice; announce.hidden = !loginNotice; }
  if ($('#modal').open) $('#modal').close();
}
async function enter(session) {
  // Retire every prior owner (including same-account re-login) before exposing
  // its replacement. The synchronous reset cannot run an old promise's finally.
  showLogin();
  loginNotice = ''; if ($('#login-notice')) $('#login-notice').hidden = true;
  state.user = session.user; state.csrf = session.csrfToken;
  const owner = sessionOwner();
  state.onboarding = session.onboarding || null;
  if (session.user.mustChangeCredentials) {
    $('#login').hidden = true; $('#console').hidden = true; $('#setup').hidden = false;
    $('#setup-form').reset(); $('#setup-error').textContent = ''; return;
  }
  $('#current-user').textContent = session.user.username;
  $('#server-origin').textContent = globalThis.location.host;
  $('#login').hidden = true; $('#setup').hidden = true; $('#console').hidden = false;
  state.route = parseHash(globalThis.location.hash);
  if (setupRequired() && !firstSetupPages.includes(state.route.page)) state.route = { page: 'proxmox', id: null };
  // An older address (#/hosts, #/sessions, #/activity …) shows its new one.
  else if (globalThis.location.hash && globalThis.location.hash !== '#') pushHash(true);
  void loadServerInfo();
  startPolling();
  await refresh();
  if (!sessionCurrent(owner)) return;
  loadPage();
  if (!setupRequired()) for (const name of ['hosts', 'hostRequests']) void load(name);
  if (hostUpdatesActive()) scheduleHostUpdatePoll();
}

// Server information and the manual server update (server-update-v1).
async function loadServerInfo() {
  if (!state.user || state.serverInfoLoading) return;
  const owner = sessionOwner(), revision = ++serverInfoRevision, badgeTicket = serverUpdateBadge.begin();
  serverInfoAt = Date.now();
  // What the last read said stays until this one answers: during an update the page is read every 1.5 s, and a
  // line that comes and goes with each read would make it jump.
  state.serverInfoLoading = true;
  if (usesServerInfo() && state.data && !state.serverUpdatePending && !updateActive.has(state.serverInfo?.update?.state)) render();
  try {
    const info = await api('/server-info');
    if (!sessionCurrent(owner) || revision !== serverInfoRevision) return;
    if (info.service !== 'multitap-broker' || typeof info.version !== 'string' || !/^\d+\.\d+\.\d+$/.test(info.version) || !['unknown', 'docker', 'systemd', 'external'].includes(info.deployment?.kind) || typeof info.update?.feedConfigured !== 'boolean' || typeof info.update?.applySupported !== 'boolean' || !Number.isFinite(Date.parse(info.checkedAt))) throw Error(t('app.10'));
    state.serverInfo = info; state.serverInfoError = ''; $('#broker-version').textContent = info.version; $('#server-version-header').textContent = info.version;
    serverUpdateBadge.update(info, badgeTicket); serverLost = false;
    // 서버 다시 시작: the server that answers now started after the one that was asked to end.
    if (state.serverRestart && info.startedAt !== state.serverRestart.startedAt) { state.serverRestart = null; notice(t('app.restarted')); }
    followServerVersion(info.version);
    // Right after 업데이트 was accepted the updater may still answer 'available' for a moment: that is not the end of it.
    const justApplied = info.update.state === 'available' && Date.now() - serverApplyAt < 8000;
    if (!updateActive.has(info.update.state) && info.update.state !== 'unavailable' && !justApplied) state.serverUpdatePending = false;
  } catch {
    if (!sessionCurrent(owner) || revision !== serverInfoRevision) return;
    serverUpdateBadge.fail(badgeTicket);
    if (!state.serverUpdatePending && !state.serverRestart) { state.serverInfo = null; $('#broker-version').textContent = '—'; $('#server-version-header').textContent = t('common.17'); }
    state.serverInfoError = state.serverUpdatePending || state.serverRestart ? t('app.11') : t('app.12');
  } finally {
    if (sessionCurrent(owner) && revision === serverInfoRevision) {
      state.serverInfoLoading = false; if (usesServerInfo() && state.data) render(); else { renderExpiryBanner(); renderUpdateBadge(); }
      clearTimeout(serverUpdateTimer); serverUpdateTimer = null;
      // A restart that gave no answer for two minutes is no longer waited for.
      if (state.serverRestart && Date.now() - state.serverRestart.at > serverRestartWaitMs) { state.serverRestart = null; notice(t('app.restartTimeout'), true); if (usesServerInfo() && state.data) render(); }
      if (state.serverUpdatePending || state.serverRestart || updateActive.has(state.serverInfo?.update?.state)) {
        serverUpdateTimer = setTimeout(() => { if (sessionCurrent(owner)) void loadServerInfo(); }, 1500); serverUpdateTimer.unref?.();
      }
    }
  }
}
const usesServerInfo = () => ['updates', 'overview', 'server-info', 'licenses'].includes(state.route.page);
// 서버 다시 시작 (서버 정보): confirmed like every action that ends connections, with 먼저 알리기 while somebody is
// signed in. The page then reads the server until one that started later answers.
const serverRestartWaitMs = 120000;
function restartServer() {
  const info = state.serverInfo;
  if (info?.restart?.supported !== true || state.serverRestart) return;
  const announce = noticesPage.announceFirst(ctx, { audience: noticesPage.everyoneAudience(ctx), text: noticesPage.announcements.serverRestart });
  const restart = async () => {
    if (state.serverRestart) return;
    const owner = sessionOwner();
    const result = await api('/server-restart', 'POST', { confirm: true });
    if (!sessionCurrent(owner)) return;
    state.serverRestart = { startedAt: typeof result?.startedAt === 'string' ? result.startedAt : info.startedAt, at: Date.now() };
    render(); void loadServerInfo();
  };
  modal(t('system.restartConfirm'), announce ? [{ node: announce.node }] : [], announce ? announce.wrap(restart) : restart, { description: t('system.restartDescription'), save: t('system.restart'), danger: true, reload: false });
}
async function checkServerUpdate() {
  if (state.serverUpdateSubmitting) return; state.serverUpdateSubmitting = true; render();
  const owner = sessionOwner();
  try { await api('/server-update/check', 'POST', {}); if (!sessionCurrent(owner)) return; state.serverUpdatePending = true; await loadServerInfo(); }
  catch (error) { if (sessionCurrent(owner)) notice(error.message, true); } finally { if (sessionCurrent(owner)) { state.serverUpdateSubmitting = false; render(); } }
}
function applyServerUpdate() {
  const info = state.serverInfo, u = info?.update;
  if (!u?.applySupported || u.state !== 'available' || state.serverUpdateSubmitting) return;
  // 먼저 알리기 (js/notices.js): offered only while somebody is signed in; unticked, the update runs as before.
  const announce = noticesPage.announceFirst(ctx, { audience: noticesPage.everyoneAudience(ctx), text: noticesPage.announcements.serverUpdate });
  const apply = async () => {
    if (state.serverUpdateSubmitting) return; state.serverUpdateSubmitting = true;
    const owner = sessionOwner();
    try { await api('/server-update/apply', 'POST', { releaseId: u.releaseId, fromVersion: info.version, confirm: true }); if (!sessionCurrent(owner)) return; state.serverUpdatePending = true; serverApplyAt = Date.now(); await loadServerInfo(); }
    finally { if (sessionCurrent(owner)) { state.serverUpdateSubmitting = false; render(); } }
  };
  modal(t('app.13'), announce ? [{ node: announce.node }] : [], announce ? announce.wrap(apply) : apply, { description: t('app.14', { version: info.version, version2: u.latest.version }), save: t('common.8') });
}

// This page runs the console of the server version it first saw (its files
// are served no-store). Another running version, applied here, elsewhere or
// by the operator, reloads it: at most once per version (sessionStorage, so
// a flapping version cannot loop) and never over unsaved work, which is asked
// about once and reloads when the work is done.
let pageVersion = null, newVersion = null, versionAsked = null, versionReloadTimer = null;
const reloadKey = version => 'multitap-admin-reloaded:' + version;
const unsavedWork = () => state.ui.serverAccess?.dirty === true || !!(state.ui.notices?.title || state.ui.notices?.body) || !!state.ui.backup?.busy || (!!state.ui.backup?.operation && !state.ui.backup.operation.done) || ($('#modal').open && !!$('#modal-fields').querySelector('input:not([type=hidden]),textarea,select'));
function reloadFor(version) {
  try { globalThis.sessionStorage.setItem(reloadKey(version), '1'); } catch { /* automatic reloads come only after a working read */ }
  globalThis.location.reload?.();
}
function followServerVersion(version) {
  pageVersion ??= version;
  newVersion = version === pageVersion ? null : version;
  if (!newVersion || versionReloadTimer) return;
  let reloaded; try { reloaded = globalThis.sessionStorage.getItem(reloadKey(version)); } catch { reloaded = undefined; }
  if (reloaded === '1') { newVersion = null; return; }
  // Without storage nothing would stop a loop, so it asks instead.
  if (reloaded === undefined || unsavedWork()) {
    if (versionAsked !== version) { versionAsked = version; notice(reloaded === undefined ? t('app.15') : t('app.16'), false, { label: t('app.17'), run: () => reloadFor(version) }); }
    return;
  }
  notice(t('app.18'));
  versionReloadTimer = setTimeout(() => { versionReloadTimer = null; if (unsavedWork()) followServerVersion(version); else reloadFor(version); }, 1500);
  versionReloadTimer.unref?.();
}

// Inventory and Proxmox synchronisation.
const setupRequired = () => state.onboarding?.proxmoxRequired === true && state.onboarding.complete !== true;
async function refresh() {
  if (state.loading || !state.user) return;
  const owner = sessionOwner(), revision = ++refreshRevision, inventoryTicket = ++inventoryRevision;
  const current = () => sessionCurrent(owner) && revision === refreshRevision;
  const latestInventory = () => current() && inventoryTicket === inventoryRevision;
  state.loading = true; $('#refresh').disabled = true;
  try {
    const inventory = await api('/inventory'); if (!latestInventory()) return;
    state.data = inventory; state.onboarding = state.data.onboarding || state.onboarding;
    if (setupRequired() && !firstSetupPages.includes(state.route.page)) state.route = { page: 'proxmox', id: null };
    if (state.data.proxmox.configured && pageDefs[state.route.page].sync) await syncProxmox(); else render();
  }
  catch (error) { if (latestInventory()) notice(error.message, true); }
  finally { if (current()) { state.loading = false; $('#refresh').disabled = false; } }
}
async function readProxmoxStatus() {
  const owner = sessionOwner();
  const result = await api('/proxmox/status');
  if (!sessionCurrent(owner)) throw sessionChanged();
  const count = result.proxmox?.unavailableCount;
  const status = { ...result.proxmox, unavailableCount: Number.isSafeInteger(count) && count >= 0 ? count : null };
  if (state.data) state.data.proxmox = status;
  if (result.onboarding) state.onboarding = result.onboarding;
  return status;
}
async function syncProxmox({ notify = true } = {}) {
  if (state.pveBusy || !state.user || !state.data?.proxmox.configured) return;
  const owner = sessionOwner();
  const firstSetup = setupRequired();
  let inventoryTicket = null;
  state.pveBusy = true; state.pveError = ''; state.pveErrorCode = null; render();
  try {
    const result = await api('/proxmox/sync', 'POST', {});
    if (!sessionCurrent(owner)) return;
    inventoryTicket = ++inventoryRevision;
    const inventory = await api('/inventory');
    if (!sessionCurrent(owner) || inventoryTicket !== inventoryRevision) return;
    state.data = inventory; state.data.proxmox = result.proxmox; state.pveLoaded = true;
    state.onboarding = result.onboarding || inventory.onboarding || state.onboarding;
    const status = await readProxmoxStatus();
    if (!sessionCurrent(owner)) return;
    if (status.unavailableCount === null && Number.isSafeInteger(result.unavailable) && result.unavailable >= 0) status.unavailableCount = result.unavailable;
    if (notify && status.unavailableCount > 0) notice(t('app.19', { unavailableCount: status.unavailableCount }), true);
    if (firstSetup && !setupRequired() && state.route.page === 'proxmox') { state.route = { page: 'overview', id: null }; pushHash(true); loadPage(); }
    return status;
  } catch (error) { if (sessionCurrent(owner) && (inventoryTicket === null || inventoryTicket === inventoryRevision)) { state.pveError = error.message; state.pveErrorCode = error.code ?? null; } }
  finally { if (sessionCurrent(owner)) { state.pveBusy = false; render(); loadPage(); } }
}

// Host program updates in progress are polled while a page shows them.
function scheduleHostUpdatePoll() {
  if (hostUpdateTimer || !state.user) return;
  const owner = sessionOwner();
  hostUpdateTimer = setTimeout(async () => {
    hostUpdateTimer = null;
    if (!sessionCurrent(owner) || !state.data || !['desktops', 'desktop', 'updates', 'overview'].includes(state.route.page)) return;
    try {
      const view = await api('/host-updates');
      if (!sessionCurrent(owner) || !state.data) return;
      state.data.hostUpdates = view;
      render();
    } catch { if (sessionCurrent(owner)) scheduleHostUpdatePoll(); }
  }, 2000);
  hostUpdateTimer.unref?.();
}
const hostUpdatesActive = () => Object.values(state.data?.hostUpdates?.desktops ?? {}).some(view => hostUpdateActive.has(view.operation?.status));

// Routing: #/page, #/desktops/<id>, #/users/<login id>, #/log/<tab>,
// #/monitoring/<tab>. Addresses of the pages that merged or moved (Host 관리,
// 접속 현황, 활동 기록, 서버 업데이트, the 지금 접속 중 tab of 기록 and the older
// assignment and audit pages) open the place that took them over.
const movedPages = { hosts: ['desktops', null], sessions: ['monitoring', null], activity: ['log', null], audit: ['log', 'admin'], assignments: ['users', null], overview: ['overview', null], home: ['overview', null], server: ['updates', null], update: ['updates', null], usage: ['monitoring', null],
  // There is no 설정 overview page; its address opens 서버 정보.
  settings: ['server-info', null], advanced: ['server-info', null], 'account-security': ['server-info', null],
  // Two more addresses without a page of their own: that a new assignment shares nothing is said in 사용자, where sharing
  // is chosen, and previews are turned on and off in the client.
  sharing: ['users', null], previews: ['server-info', null] };
// What stood under #/settings/<name>.
const movedSettings = { update: 'updates', updates: 'updates', server: 'updates', downloads: 'downloads', proxmox: 'proxmox', 'server-access': 'server-access', sharing: 'users', previews: 'server-info', licenses: 'licenses', 'server-info': 'server-info' };
function parseHash(hash = '') {
  const parts = String(hash ?? '').replace(/^#\/?/, '').split('/').filter(Boolean);
  const [head, ...rest] = parts;
  let id = null;
  if (rest.length) { try { id = decodeURIComponent(rest.join('/')); } catch { id = null; } }
  if (head === 'desktops' && id) return { page: 'desktop', id };
  if (head === 'users' && id) return { page: 'user', id };
  if (head === 'log') return id === 'live' ? { page: 'monitoring', id: null } : { page: 'log', id: monitor.logTabs.includes(id) && id !== monitor.logTabs[0] ? id : null };
  if (head === 'monitoring') return { page: 'monitoring', id: usage.usageTabs.includes(id) && id !== usage.usageTabs[0] ? id : null };
  if (head === 'settings' && id && Object.hasOwn(movedSettings, id)) return { page: movedSettings[id], id: null };
  if (Object.hasOwn(movedPages, head)) return { page: movedPages[head][0], id: movedPages[head][1] };
  // Only a page of this console: #/constructor or #/__proto__ is no page.
  return Object.hasOwn(pageDefs, head) && !['desktop', 'user'].includes(head) ? { page: head, id: null } : { page: 'overview', id: null };
}
function hashFor(page, id = null) {
  if (page === 'desktop') return '#/desktops/' + encodeURIComponent(id);
  if (page === 'user') return '#/users/' + encodeURIComponent(id);
  if (page === 'log' || page === 'monitoring') return id ? `#/${page}/` + encodeURIComponent(id) : '#/' + page;
  return page === 'overview' ? '#/' : '#/' + page;
}
function pushHash(replace = false) {
  const target = hashFor(state.route.page, state.route.id);
  if (globalThis.location?.hash === target) return;
  try { const history = globalThis.history; if (history?.pushState) history[replace ? 'replaceState' : 'pushState'](null, '', target); } catch { /* A sandboxed page keeps working without history. */ }
}
// Opening an item from its list adds one step to the history, and leaving the
// window takes that step back, so 닫기, X, Esc, the backdrop and the browser's
// Back all end on the list as it was. A window reached another way (a link
// from another page, a typed address) is replaced by its list instead.
function navigate(page, id = null, { push = true } = {}) {
  // A link to a page that moved (a page module may still name 설정) opens where it went.
  if (!Object.hasOwn(pageDefs, page)) [page, id] = Object.hasOwn(movedPages, page) ? movedPages[page] : ['overview', null];
  if (setupRequired() && !firstSetupPages.includes(page)) { page = 'proxmox'; id = null; }
  const from = state.route;
  const withinWorkspace = ['desktop', 'desktops'].includes(page) && ['desktop', 'desktops'].includes(from.page);
  const changed = from.page !== page || from.id !== id;
  const closing = changed && pageDefs[from.page].parent === page, opening = changed && pageDefs[page].parent === from.page;
  // Focus goes back to the row the window was opened from.
  const back = closing ? shownWindow()?.getAttribute('data-return-focus') : null, refocus = back ? { key: back } : null;
  state.route = { page, id };
  if (changed) notice('');
  if (push && closing && detailFromList && typeof globalThis.history?.back === 'function') { try { globalThis.history.back(); } catch { pushHash(true); } }
  else if (push) pushHash(closing);
  if (changed) detailFromList = push && opening;
  setNav(false, { restoreFocus: !changed });
  render({ focus: changed, refocus });
  enterPage({ sync: !withinWorkspace });
}
function enterPage({ sync = true } = {}) {
  if (!state.user || !state.data) return;
  const def = pageDefs[state.route.page];
  if (usesServerInfo()) void loadServerInfo();
  if (sync && state.data.proxmox.configured && def.sync && !setupRequired()) void syncProxmox();
  loadPage();
  if (hostUpdatesActive()) scheduleHostUpdatePoll();
}
function link(text, page, id = null, attrs = {}) {
  return el('a', { href: hashFor(page, id), ...attrs, onclick: event => { if (event.metaKey || event.ctrlKey || event.shiftKey) return; event.preventDefault?.(); navigate(page, id); } }, text);
}

// Console frame: navigation state, breadcrumbs, document title and badges.
// The narrow drawer is modal: the underlying operator actions cannot receive
// focus while it is open. Keep any pre-existing inert state when it closes.
const drawerMedia = globalThis.matchMedia?.('(max-width:1000px)');
const drawerInert = new Map();
const drawerModal = () => $('#console').classList.contains('nav-open') && drawerMedia?.matches !== false;
const drawerControls = () => [...$('#sidebar').querySelectorAll('a[href],button,input,select,textarea,[tabindex]')]
  .filter(node => !node.disabled && node.getAttribute('tabindex') !== '-1' && !node.closest('[hidden],[inert]') &&
    (typeof node.getClientRects !== 'function' || node.getClientRects().length));
function setNav(open, { restoreFocus = false } = {}) {
  open = !!open && drawerMedia?.matches !== false;
  const wasOpen = $('#console').classList.contains('nav-open');
  $('#console').classList.toggle('nav-open', open);
  $('#nav-toggle').setAttribute('aria-expanded', String(open));
  $('#nav-scrim').hidden = !open;
  if (open && !wasOpen) {
    for (const node of [$('#console > .topbar'), $('#console > .main'), $('#console > .skip-link')].filter(Boolean)) {
      drawerInert.set(node, { attribute: node.hasAttribute('inert'), property: node.inert === true });
      node.setAttribute('inert', ''); node.inert = true;
    }
  } else if (!open) {
    for (const [node, previous] of drawerInert) {
      if (previous.attribute) node.setAttribute('inert', ''); else node.removeAttribute('inert');
      node.inert = previous.property;
    }
    drawerInert.clear();
  }
  if (open && !wasOpen) ($('#sidebar .nav a.active') ?? $('#sidebar .nav a'))?.focus?.();
  if (!open && wasOpen && restoreFocus) $('#nav-toggle').focus?.();
}
drawerMedia?.addEventListener?.('change', event => {
  if (globalThis.document === ownDocument && !event.matches) setNav(false);
});
// Expiry banner on every page: the Proxmox API token (inventory) from 30 days
// before expiry, red from 7; the Proxmox certificate (pve-health-v1: refused,
// or a pinned one from 30 days, red from 7; overview.proxmoxCertificateNotice);
// the TLS certificate the server presents (server-info, tls-expiry-v1) from 14
// days, red from 3. Rebuilt only when it changes (it is an alert).
const daysUntil = iso => { const time = Date.parse(iso); return Number.isFinite(time) ? Math.ceil((time - Date.now()) / 86400000) : null; };
let expiryBannerKey = '';
function renderExpiryBanner() {
  const items = [], proxmox = state.data?.proxmox, tls = state.serverInfo?.tlsCertificate;
  const tokenDays = proxmox?.configured ? daysUntil(proxmox.expiresAt) : null, tlsDays = tls?.expiresAt ? daysUntil(tls.expiresAt) : null;
  const certificate = overview.proxmoxCertificateNotice(proxmox);
  if (certificate) items.push([certificate.tone, 'shield', certificate.title, certificate.text, 'proxmox']);
  if (tokenDays !== null && tokenDays <= 30) items.push([tokenDays <= 7 ? 'bad' : 'warn', 'key', tokenDays <= 0 ? t('common.18') : t('common.19', { tokenDays }), t('app.20'), 'proxmox']);
  if (tlsDays !== null && tlsDays <= 14) items.push([tlsDays <= 3 ? 'bad' : 'warn', 'shield', tlsDays <= 0 ? t('app.21') : t('app.22', { tlsDays }), t('app.23'), null]);
  const key = JSON.stringify(items), node = $('#expiry-banner');
  if (!node || key === expiryBannerKey) return;
  expiryBannerKey = key; node.hidden = !items.length;
  node.replaceChildren(...items.map(([tone, iconName, title, text, page]) => {
    const body = [el('span', { class: 'expiry-icon' }, icon(iconName)), el('span', { class: 'expiry-text' }, el('strong', {}, title), el('small', {}, text))];
    return page ? link([...body, icon('chevron', 'icon expiry-go')], page, null, { class: 'expiry-item tone-' + tone }) : el('div', { class: 'expiry-item tone-' + tone }, body);
  }));
}
// 업데이트 carries how many things can be updated now: a verified newer server
// (0 or 1) and the desktops with a Host update; nothing to update, no badge.
function renderUpdateBadge() {
  const badge = $('[data-nav-badge=updates]'), offered = serverUpdateBadge.offered();
  if (!badge) return;
  const counts = state.user && state.data && !setupRequired() ? system.updateCounts(ctx, offered) : { server: 0, hosts: 0, total: 0 };
  badge.hidden = !counts.total; badge.textContent = counts.total ? String(counts.total) : '';
  const label = t('app.24') + [counts.server ? t('app.25', { offered }) : null, counts.hosts ? t('app.26', { hosts: counts.hosts }) : null].filter(Boolean).join(', ');
  if (counts.total) { badge.setAttribute('aria-label', label); badge.setAttribute('title', label); } else { badge.removeAttribute('aria-label'); badge.removeAttribute('title'); }
}
function updateShell() {
  serverUpdateBadge.render();
  renderExpiryBanner();
  const route = state.route, locked = setupRequired();
  const active = navOf(route.page);
  document.querySelectorAll('#sidebar [data-page]').forEach(node => {
    const page = node.dataset.page, current = page === active;
    // Until Proxmox is connected only the two pages of the first setup are offered.
    node.hidden = locked && !firstSetupPages.includes(page); node.classList.toggle('active', current);
    if (current) node.setAttribute('aria-current', 'page'); else node.removeAttribute('aria-current');
  });
  const security = $('#nav-account-security'); if (security) security.hidden = locked;
  // A group with nothing to offer shows neither its heading nor its divider.
  document.querySelectorAll('#sidebar .nav-group').forEach(group => { group.hidden = ![...group.querySelectorAll('[data-page],.nav-action')].some(node => !node.hidden); });
  // Until Proxmox is connected there is nothing to search.
  const search = $('.global-search'); if (search) search.hidden = locked || ['desktops', 'users'].includes(active);
  // 홈 carries the number of things in 지금 할 일.
  const badge = $('[data-nav-badge=home]'), todo = state.data && !locked ? overview.todoCount(ctx) : 0;
  if (badge) { badge.hidden = !todo; badge.textContent = todo ? String(todo) : ''; badge.setAttribute('aria-label', t('app.27', { todo })); }
  renderUpdateBadge();
  const title = state.data ? pageTitle(route) : pageDefs[route.page].title;
  const text = typeof title === 'string' ? title : '';
  const crumbs = [];
  if (pageDefs[route.page].parent) crumbs.push(el('li', {}, link(pageDefs[pageDefs[route.page].parent].title, pageDefs[route.page].parent)));
  // A settings page stands under the sidebar's 설정 heading, which is not a page.
  else if (pageDefs[route.page].group && !locked) crumbs.push(el('li', { class: 'crumb-group' }, pageDefs[route.page].group));
  crumbs.push(el('li', {}, el('strong', { id: 'breadcrumb', 'aria-current': 'page' }, text)));
  $('#crumbs').replaceChildren(...crumbs);
  document.title = t('app.29', { value: text || t('app.28') });
}
// The sidebar lists the settings pages. The first setup also shows its three
// as a bar above the page, in the sidebar's order.
function sectionNavigation() {
  const page = state.route.page;
  if (!setupRequired() || !firstSetupPages.includes(page)) return null;
  return el('nav', { class: 'section-navigation', 'aria-label': t('app.30') },
    [['server-access', t('common.12')], ['proxmox', t('common.10')], ['backup', t('common.13')]].map(([key, label]) => link(label, key, null, { class: 'section-link' + (page === key ? ' selected' : ''), 'aria-current': page === key ? 'page' : null })));
}
// Re-rendering keeps open disclosures and the focused control.
function render({ focus = false, refocus = null } = {}) {
  if (globalThis.document !== ownDocument) return;
  updateShell();
  const content = $('#content');
  content.dataset.page = state.route.page;
  // First load: the page's shape in placeholders until the inventory arrives.
  if (!state.data) {
    shutDetail();
    const bone = (cls = '') => el('span', { class: 'bone' + (cls ? ' ' + cls : '') });
    content.replaceChildren(el('div', { class: 'skeleton', role: 'status', 'aria-busy': 'true' }, el('span', { class: 'visually-hidden' }, t('app.31')),
      el('div', { class: 'skeleton-head', 'aria-hidden': 'true' }, bone(), bone()),
      el('div', { class: 'skeleton-tiles', 'aria-hidden': 'true' }, [0, 1, 2, 3].map(() => el('div', { class: 'skeleton-card' }, bone(), bone('is-big'), bone()))),
      el('div', { class: 'skeleton-card is-table', 'aria-hidden': 'true' }, bone(), bone(), bone(), bone(), bone())));
    return;
  }
  const active = document.activeElement, activeKey = active?.getAttribute?.('data-focus') ?? null;
  // A control without a key of its own is found again by where it stands (focusMark).
  const activeMark = activeKey ? null : focusMark(active, content);
  if (active?.tagName === 'H1' && content.contains?.(active)) focus = true;
  const wasOpen = detailOpen(), inDetail = wasOpen && detail.contains?.(active) === true, onTitle = inDetail && active?.id === 'detail-title';
  const selection = activeKey && typeof active.selectionStart === 'number' ? [active.selectionStart, active.selectionEnd, active.selectionDirection] : null;
  const scroll = new Map([...content.querySelectorAll('[data-scroll-key]')].map(node => [node.dataset.scrollKey, [node.scrollLeft, node.scrollTop]]));
  // Native disclosure toggle events may still be queued when a control
  // immediately redraws its section. Snapshot the actual DOM before removal.
  for (const node of content.querySelectorAll('details[data-keep-open]')) state.expanded.set(node.getAttribute('data-keep-open'), node.hasAttribute('open'));
  const nodes = [sectionNavigation(), pageDefs[state.route.page].render(ctx, state.route)].flat(Infinity).filter(node => node !== null && node !== undefined && node !== false);
  // A page that shows one item of its list returns that item's window among its nodes (dom.js detailWindow). The
  // modal stays the same element while it is open (it is in the top layer): the page under it and the window in
  // it are replaced. A tab of a page may return the window inside its panel: it is taken out of there.
  const frame = nodes.find(node => node.classList?.contains('win-frame')) ?? nodes.map(node => node.querySelector?.('.win-frame')).find(Boolean) ?? null;
  frame?.remove();
  // 조회 조건 stays the very same element while nothing in it changed, so a refresh does not close a list of
  // choices that is open or move the caret. The box may stand in the page or in a tab panel of the page (which
  // then stays too); everything else is replaced.
  const boxes = new Map([...content.querySelectorAll('.query-box[aria-label]')].filter(node => !detail.contains(node)).map(node => [node.getAttribute('aria-label'), node]));
  const isBox = node => node?.matches?.('.query-box[aria-label]') === true;
  const sameBox = fresh => { const old = boxes.get(fresh.getAttribute('aria-label')); return old && boxState(old) === boxState(fresh) ? old : fresh; };
  const page = nodes.filter(node => node !== frame).map(node => {
    if (isBox(node)) { const kept = sameBox(node); return kept.parentNode === content || kept === node ? kept : node; }
    const inner = node.id ? [...node.children ?? []].find(isBox) : null, kept = inner ? sameBox(inner) : null, holder = kept && kept !== inner ? kept.parentNode : null;
    if (!holder || holder.parentNode !== content || holder.id !== node.id || holder.tagName !== node.tagName) return node;
    for (const name of holder.getAttributeNames?.() ?? []) if (!node.hasAttribute(name)) holder.removeAttribute(name);
    for (const name of node.getAttributeNames?.() ?? []) holder.setAttribute(name, node.getAttribute(name));
    place(holder, [...node.childNodes].map(child => child === inner ? kept : child), new Set([kept]));
    return holder;
  });
  if (!frame) shutDetail();
  place(content, frame ? [...page, detail] : page, new Set([...page.filter(node => node.parentNode === content), ...(frame ? [detail] : [])]));
  if (frame) {
    const old = shownWindow(); if (old) old.replaceWith(frame); else detail.prepend(frame);
    if (!wasOpen) { if (typeof detail.showModal === 'function') detail.showModal(); else detail.setAttribute('open', ''); placeNotice(); }
  }
  followWindowStep(frame);
  const opened = !wasOpen && detailOpen();
  for (const node of content.querySelectorAll('[data-scroll-key]')) { const position = scroll.get(node.dataset.scrollKey); if (position) { node.scrollLeft = position[0]; node.scrollTop = position[1]; } }
  for (const node of content.querySelectorAll('details[data-keep-open]')) {
    const key = node.getAttribute('data-keep-open');
    if (state.expanded.has(key)) { if (state.expanded.get(key)) node.setAttribute('open', ''); else node.removeAttribute('open'); }
  }
  // A window that just opened takes the focus on its title; while it is open the focus stays inside it.
  const target = opened ? null : refocus ?? (activeKey ? { key: activeKey, range: selection } : null);
  const node = target ? [...content.querySelectorAll('[data-focus]')].find(candidate => candidate.getAttribute('data-focus') === target.key)
    : opened || focus ? null : findMark(activeMark, content);
  if (node) {
    if (document.activeElement !== node) node.focus?.();
    if (target?.range && ['search', 'text', 'url', 'textarea'].includes(node.type)) node.setSelectionRange?.(target.range[0], target.range[1], target.range[2] || 'none');
  } else if (opened || onTitle || (focus && detailOpen())) detail.querySelector('#detail-title')?.focus?.();
  else if (inDetail && detailOpen()) detail.querySelector('.win-body')?.focus?.();
  else if (focus) content.querySelector('h1')?.focus?.();
}
// Puts `fresh` in `parent` in that order without taking out the nodes of `keep` that are already there.
function place(parent, fresh, keep) {
  for (const node of [...parent.childNodes]) if (!keep.has(node)) node.remove();
  let cursor = parent.firstChild;
  for (const node of fresh) { if (node.parentNode === parent && keep.has(node)) cursor = node.nextSibling; else parent.insertBefore(node, cursor); }
}
// What a 조회 조건 box shows and holds: its names, its choices and what is chosen or typed.
const boxState = box => [...box.querySelectorAll('label,select,input,button,.query-name')].map(node => {
  const key = node.getAttribute('data-focus') ?? '', off = node.disabled ? '!' : '';
  if (node.tagName === 'SELECT') return `S${key}${off}=${[...node.querySelectorAll('option')].map(option => (option.selected ? '*' : '') + option.value + ':' + option.textContent).join(',')}`;
  if (node.tagName === 'INPUT') return `I${key}${off}=${['checkbox', 'radio'].includes(node.type) ? node.checked : node.value}|${node.getAttribute('min') ?? ''}|${node.getAttribute('max') ?? ''}`;
  if (node.tagName === 'BUTTON') return `B${key}${off}=${node.textContent}|${node.getAttribute('aria-pressed') ?? ''}`;
  return `L${node.className}=${[...node.childNodes].filter(child => child.nodeType === 3).map(child => child.textContent).join('')}`;
}).join('\n') + '\n' + box.className + [...box.querySelectorAll('small')].map(node => node.textContent).join('|');
// Where a control without a data-focus key stands: the row it is in (by that row's own key or first words), what
// it is and says, and its place among the like there. A redraw finds the same control again with it.
const markable = 'button,a[href],select,input,textarea,summary,[tabindex]';
const markOf = (node, root) => {
  const row = node.closest?.('tr,li,.row-line,.todo-row,.todo-item,.request-card'), inRow = row && root.contains?.(row) && row !== root ? row : null;
  const where = inRow ? inRow.querySelector?.('[data-focus]')?.getAttribute('data-focus') ?? inRow.querySelector?.('th,td,strong')?.textContent.trim().slice(0, 80) ?? '' : '';
  const says = ['SELECT', 'INPUT', 'TEXTAREA'].includes(node.tagName) ? '' : node.textContent.trim().slice(0, 80);
  return [where, node.tagName, node.getAttribute('type') ?? '', node.getAttribute('name') ?? '', node.getAttribute('aria-label') ?? '', node.getAttribute('href') ?? '', node.id ?? '', node.className ?? '', says].join('\u0001');
};
function focusMark(node, root) {
  if (!node || node === root || root.contains?.(node) !== true || node.matches?.(markable) !== true) return null;
  const mark = markOf(node, root), like = [...root.querySelectorAll(markable)].filter(other => markOf(other, root) === mark);
  return { mark, index: like.indexOf(node), count: like.length };
}
function findMark(found, root) {
  if (!found) return null;
  const like = [...root.querySelectorAll(markable)].filter(node => markOf(node, root) === found.mark);
  // The same number of the like as before: the same place. Otherwise only an unmistakable one.
  return like.length === found.count ? like[found.index] ?? null : like.length === 1 ? like[0] : null;
}
// A window that is not an address (공지 쓰기, a sent notice, one person's connected time, one connection's charts)
// is a step of the history all the same: opening it adds one with the same address, so the browser's Back
// closes the window and nothing else, and closing it another way takes that step back.
let windowStep = null, followingHistory = false;
const stepOf = () => { try { return globalThis.history?.state?.multitapWindow ?? null; } catch { return null; } };
function followWindowStep(frame) {
  const { page, id } = state.route, addressed = !!pageDefs[page].parent;
  if (addressed || (windowStep && (windowStep.page !== page || windowStep.id !== id))) windowStep = null;
  if (addressed) return;
  if (frame && !windowStep) {
    windowStep = { page, id };
    if (!followingHistory) try { globalThis.history?.pushState?.({ multitapWindow: frame.getAttribute('data-window') ?? '' }, '', hashFor(page, id)); } catch { /* A sandboxed page keeps working without history. */ }
  } else if (!frame && windowStep) {
    windowStep = null;
    if (!followingHistory && stepOf() !== null && typeof globalThis.history?.back === 'function') try { globalThis.history.back(); } catch { /* as above */ }
  }
}
// A form or confirmation, and 계정 보안, do not outlive the page they were opened on (the browser's Back).
function closeDialogs() {
  const dialog = $('#modal');
  if (dialog.open && !modalBusy) { modalRevision++; $('#modal-form').reset(); $('#modal-fields').replaceChildren(); dialog.close(); }
  for (const node of document.querySelectorAll('dialog.account-security-dialog')) if (node.open) node.querySelector('[data-close-security]')?.click?.();
}
// The detail window (창): one item of a list, in a centred modal over that list. The page builds the window
// (dom.js detailWindow) and returns it among its nodes; render() shows it in this modal, and closes the modal
// when the page returns none. The modal lives in #content, after the page, so what a page reads from its own
// content includes it. A form or a confirmation (#modal) opens above it.
const detail = el('dialog', { id: 'detail', 'aria-labelledby': 'detail-kind detail-title' });
let detailFromList = false, detailClosedHere = false;
const detailOpen = () => detail.hasAttribute('open');
const shownWindow = () => [...detail.children].find(node => node.classList.contains('win-frame')) ?? null;
const noticeHome = $('#notice').parentNode;
// The toast stands in the open window: there it is above the dimmed page and its buttons can be pressed.
function placeNotice() {
  const node = $('#notice'), home = detailOpen() ? detail : noticeHome;
  if (node.parentNode === home) return;
  if (home === detail) detail.append(node); else noticeHome.insertBefore(node, $('#content'));
}
// A window opens on its first tab: the tab that was open is forgotten with it.
function shutDetail() {
  state.ui.detailTab = null; shownWindow()?.remove();
  if (detailOpen()) { if (typeof detail.close === 'function') { detailClosedHere = true; detail.close(); } else detail.removeAttribute('open'); }
  placeNotice();
}
// Leaving the window is what its own X does: the list's address for an item of a list, the page's own state otherwise.
const closeDetail = () => shownWindow()?.querySelector('.win-close')?.click();
// Esc closes the modal itself (its close event follows); the address then goes to the list. A close made here,
// because the page changed, needs nothing more. Pressing the dimmed page around the window closes it too.
detail.addEventListener('close', () => { if (detailClosedHere) detailClosedHere = false; else if (globalThis.document === ownDocument) closeDetail(); });
let detailPressed = false;
detail.addEventListener('mousedown', event => { detailPressed = event.target === detail; });
detail.addEventListener('click', event => { if (detailPressed && event.target === detail && shownWindow()?.windowDirty?.() !== true) closeDetail(); detailPressed = false; });
// Space opens the row whose name has the focus, as Enter does.
$('#content').addEventListener('keydown', event => {
  if (event.key !== ' ' || event.defaultPrevented || event.target?.matches?.('.board td.is-name a') !== true) return;
  event.preventDefault(); event.target.click();
});
$('#content').addEventListener('toggle', event => { const key = event.target?.getAttribute?.('data-keep-open'); if (key) state.expanded.set(key, event.target.hasAttribute('open')); }, true);

// A filter text field that survives re-rendering, including Korean IME
// composition (no re-render until the composition ends).
function searchInput({ key, value: current = '', placeholder, label, onChange, disabled = false, className = '' }) {
  const handler = event => {
    if (event.isComposing || !event.target.isConnected) return;
    onChange(event.target.value);
    render({ refocus: { key, range: [event.target.selectionStart ?? event.target.value.length, event.target.selectionEnd ?? event.target.value.length, event.target.selectionDirection] } });
  };
  return el('span', { class: 'search-field' + (className ? ' ' + className : '') }, icon('search'),
    el('input', { type: 'search', value: current, placeholder, 'aria-label': label, 'data-focus': key, disabled, oninput: handler, oncompositionend: handler }));
}
// Page header: the title, then one short sentence, and the page's actions on
// the right (the main one last). A list page keeps its numbers and actions
// above its table instead (dom.js listHead).
function pageHeader({ title, description = '', actions = [], meta = null, back = null }) {
  // 설정 is a sidebar heading now, not a page: a way back to it is not shown.
  if (back?.getAttribute?.('href') === '#/settings') back = null;
  return el('header', { class: 'page-header' },
    el('div', { class: 'page-heading-text' }, back, el('div', { class: 'page-title-row' }, el('h1', { tabindex: '-1' }, title)), description ? el('p', { class: 'page-description' }, description) : null, meta ? el('div', { class: 'page-meta' }, meta) : null),
    actions.filter(Boolean).length ? el('div', { class: 'page-actions' }, actions) : null);
}

// Dialogs: focused forms and confirmations only. `info` dialogs present
// information and their single action closes them.
function modal(title, descriptors, action, { save = t('common.20'), description = '', danger = false, reload = true, size = '', info = false, safeFocus = false, submitControls = [], confirmText = null } = {}) {
  const revision = ++modalRevision, owner = sessionOwner();
  const current = () => sessionCurrent(owner) && revision === modalRevision;
  const dialog = $('#modal'), form = $('#modal-form'); form.reset(); $('#modal-title').textContent = title; $('#modal-error').textContent = '';
  dialog.className = size ? 'is-' + size : ''; $('#modal-cancel').hidden = info;
  const nodes = descriptors.map(field => {
    if (field.node) return field.node;
    // One plain checkbox (sent as 'yes' only when ticked), its words beside it.
    if (field.checkbox) return el('label', { class: 'modal-check' }, el('input', { type: 'checkbox', name: field.name, value: 'yes' }), el('span', {}, field.label), field.hint ? el('small', { class: 'field-hint' }, field.hint) : null);
    let input;
    if (field.link) input = el('a', { class: 'download-link', href: field.link, download: true }, icon('download'), el('span', {}, field.value));
    else if (field.command) input = commandBox(el, button, field.value);
    else if (field.readonly) input = el('div', { class: 'readonly-field' }, field.value);
    else if (field.options) input = el('select', { name: field.name, required: true }, field.options.map(([key, text]) => el('option', { value: key, selected: String(key) === String(field.value) }, text)));
    else input = el(field.multiline ? 'textarea' : 'input', { name: field.name, type: field.type || 'text', required: field.required !== false, maxlength: field.max || 200, minlength: field.min, autocomplete: field.type === 'password' ? 'new-password' : 'off', placeholder: field.placeholder || '' });
    if (!field.options && !field.readonly && !field.command && !field.link) input.value = field.value || '';
    return el('label', {}, field.label, input, field.hint ? el('small', { class: 'field-hint' }, field.hint) : null);
  });
  $('#modal-fields').replaceChildren(description ? el('p', { class: 'modal-description' }, description) : '', ...nodes);
  // A typed confirmation (confirmText: { expected, label }, or { blocked } when nothing may be done): the
  // dialog's button stays off, and Enter does nothing, until the field holds exactly the expected text
  // (spaces at both ends aside).
  const confirmField = confirmText && !confirmText.blocked ? el('input', { name: 'confirmText', type: 'text', autocomplete: 'off', spellcheck: 'false', 'data-confirm-text': '' }) : null;
  if (confirmField) $('#modal-fields').append(el('label', { class: 'confirm-text' }, confirmText.label, confirmField));
  const confirmed = () => !confirmText || (!!confirmField && confirmField.value.trim() === confirmText.expected);
  const gate = () => { if (confirmText) $('#modal-save').disabled = !confirmed(); };
  // Compare only editable form values. Merely opening an unchanged form or a
  // read-only guide must not cause an unload warning.
  const initialFields = [...$('#modal-fields').querySelectorAll('input,textarea,select')].filter(node => !node.disabled && !node.readOnly)
    .map(node => ({ node, value: node.value, checked: node.checked }));
  modalDraftDirty = () => dialog.open && initialFields.some(({ node, value, checked }) =>
    ['checkbox', 'radio'].includes(node.type) ? node.checked !== checked : node.value !== value);
  $('#modal-save').textContent = save; $('#modal-save').className = danger ? 'danger' : 'primary';
  const controls = [$('#modal-save'), $('#modal-close'), $('#modal-cancel'), ...submitControls];
  controls.forEach(node => node.disabled = false); gate();
  let submitting = false;
  confirmField?.addEventListener('input', () => { if (!submitting) gate(); });
  const clearError = () => {
    $('#modal-error').textContent = '';
    for (const node of form.querySelectorAll('[data-modal-invalid]')) {
      node.removeAttribute('aria-invalid'); node.removeAttribute('data-modal-invalid');
      const ids = (node.getAttribute('aria-describedby') || '').split(/\s+/).filter(id => id && id !== 'modal-error');
      if (ids.length) node.setAttribute('aria-describedby', ids.join(' ')); else node.removeAttribute('aria-describedby');
    }
  };
  const showError = (message, fieldName = null) => {
    const error = $('#modal-error'); error.textContent = message; error.setAttribute('tabindex', '-1');
    const field = fieldName ? [...form.querySelectorAll('[name]')].find(node => node.name === fieldName) : null;
    if (field) {
      field.setAttribute('aria-invalid', 'true'); field.setAttribute('data-modal-invalid', '');
      field.setAttribute('aria-describedby', [...new Set((field.getAttribute('aria-describedby') || '').split(/\s+/).filter(Boolean).concat('modal-error'))].join(' '));
    }
    const target = field ?? error;
    target.focus?.();
    globalThis.requestAnimationFrame?.(() => {
      if (current() && dialog.open && target.isConnected && globalThis.document.activeElement === target) {
        target.scrollIntoView?.({ block: 'nearest', inline: 'nearest' });
      }
    });
  };
  dialog.oncancel = event => { if (submitting) event.preventDefault(); else if (current()) modalRevision++; };
  const run = async (submitAction, extraControls = []) => {
    if (submitting || !current() || !dialog.open || !confirmed()) return false; clearError(); submitting = true; modalBusy = true;
    const ownedControls = [...new Set([...controls, ...extraControls])]; ownedControls.forEach(node => node.disabled = true);
    const input = Object.fromEntries(new FormData(form));
    if ('passwordConfirmation' in input && input.password !== input.passwordConfirmation) { showError(messages.password_confirmation_mismatch, 'passwordConfirmation'); submitting = false; modalBusy = false; ownedControls.forEach(node => node.disabled = false); return false; }
    try { await submitAction(input); if (!current() || !dialog.open) return false; form.reset(); dialog.close(); $('#modal-fields').replaceChildren(); if (reload) await refresh(); else render(); return true; }
    catch (error) { if (current()) { if (dialog.open) showError(error.message); else notice(error.message, true); } }
    finally { modalBusy = false; if (current()) { submitting = false; ownedControls.forEach(node => node.disabled = false); gate(); } }
  };
  activeModalSubmission = { revision, run };
  form.onsubmit = event => { event.preventDefault(); return run(action); };
  dialog.showModal();
  // Focus starts in the first field; a confirmation starts on its safe choice.
  const first = $('#modal-fields').querySelector?.('input:not([type=hidden]):not(:disabled),select:not(:disabled),textarea:not(:disabled)');
  (safeFocus && !info ? $('#modal-cancel') : first ?? (danger && !info ? $('#modal-cancel') : $('#modal-save')))?.focus?.();
  return revision;
}
function submitModal(revision, action, extraControls = []) {
  return activeModalSubmission?.revision === revision ? activeModalSubmission.run(action, extraControls) : Promise.resolve(false);
}
function confirmAction(title, description, action, save = t('common.21'), danger = false, { confirmText = null } = {}) { return modal(title, [], action, { description, save, danger, confirmText }); }
async function change(path, method, data, text = t('app.32')) { const owner = sessionOwner(), revision = modalRevision; const result = await api(path, method, data); if (!sessionCurrent(owner)) throw sessionChanged(); if (revision === modalRevision) notice(text); return result; }
const modalCurrent = revision => $('#modal').open && revision === modalRevision;

const desktopById = id => state.data?.desktops.find(d => d.id === id) ?? null;
const userByName = name => state.data?.users.find(u => u.username === name) ?? null;
const userById = id => state.data?.users.find(u => u.id === id) ?? null;
const ctx = { state, api, render, navigate, link, hashFor, modal, confirmAction, modalCurrent, submitModal, pageAction, notice, change, refresh, syncProxmox, readProxmoxStatus, setupRequired, load, resource, value,
  loadServerInfo, checkServerUpdate, applyServerUpdate, restartServer, scheduleHostUpdatePoll, searchInput, pageHeader, desktopById, userByName, userById, productRelease,
  announceAtLogin: text => { loginNotice = typeof text === 'string' ? text : ''; },
  openAccountSecurity: () => { if (state.user && !setupRequired()) void accountSecurityController.open(); } };

// The first setup screen also offers a restore (js/backup.js), in its own card
// beside the form that makes the first administrator.
function showSetupRestore(open) {
  const card = $('#setup-restore'), form = $('#setup-form');
  if (!card || !form) return;
  card.hidden = !open; form.hidden = !!open;
  if (!open) { card.replaceChildren(); return; }
  const back = () => showSetupRestore(false);
  backupPage.renderSetupRestore(ctx, card, { back });
  // A fresh Docker server says so once its answer is in (the card is drawn again only if nothing was entered yet).
  void backupPage.loadSetupStatus(ctx, () => { if (!card.hidden) backupPage.renderSetupRestore(ctx, card, { back }); });
}

// Global search: VMs by ID, name or node and accounts by login ID or notes.
const searchBox = $('#global-search'), searchList = $('#global-search-results');
let searchActive = -1, searchItems = [];
function searchMatches(query) {
  const q = query.trim().toLowerCase();
  if (!q || !state.data) return [];
  const registered = hosts.registeredHostIds(ctx);
  const vms = state.data.desktops.filter(d => registered?.has(d.id) && (d.kind !== 'pc' || d.localPcApproved === true) && `${d.kind === 'pc' ? t('common.22') : d.pve_vmid} ${d.name} ${d.pve_node ?? ''}`.toLowerCase().includes(q)).sort((a, b) => Number(a.pve_vmid) - Number(b.pve_vmid)).slice(0, 6)
    .map(d => { const status = hosts.desktopStatus(ctx, d);
      return { page: 'desktop', id: d.id, iconName: 'monitor', title: d.kind === 'pc' ? t('common.6', { name: d.name }) : `VM ${d.pve_vmid} · ${d.name}`, detail: [d.pve_node, status.cause === 'silent' ? t('common.23') : status.why].filter(Boolean).join(' · '), badge: [status.label, status.tone] }; });
  const accounts = state.data.users.filter(u => `${u.username} ${u.notes ?? ''}`.toLowerCase().includes(q)).slice(0, 6)
    .map(u => ({ page: 'user', id: u.username, iconName: 'user', title: u.username, detail: [u.role === 'admin' ? t('common.24') : t('common.7'), u.disabled ? t('common.25') : null, u.notes ? u.notes.split('\n')[0] : null].filter(Boolean).join(' · ') }));
  return [...vms, ...accounts];
}
function renderSearch() {
  const query = searchBox.value ?? '';
  searchItems = searchMatches(query);
  const open = !!query.trim() && searchBox.getAttribute('data-open') === 'true';
  searchList.hidden = !open; searchBox.setAttribute('aria-expanded', String(open));
  if (searchActive >= searchItems.length) searchActive = searchItems.length - 1;
  if (searchActive >= 0 && open) searchBox.setAttribute('aria-activedescendant', 'search-option-' + searchActive); else searchBox.removeAttribute('aria-activedescendant');
  searchList.replaceChildren(...(searchItems.length ? searchItems.map((item, index) => el('div', { id: 'search-option-' + index, role: 'option', class: 'search-option' + (index === searchActive ? ' active' : ''), 'aria-selected': String(index === searchActive),
    onmousedown: event => event.preventDefault(), onclick: () => chooseSearch(index) }, icon(item.iconName), el('span', {}, el('strong', {}, item.title), item.detail ? el('small', {}, item.detail) : null), item.badge ? stateDot(item.badge[0], item.badge[1]) : null)) : [el('div', { class: 'search-empty', role: 'option', 'aria-disabled': 'true', 'aria-selected': 'false' }, t('app.33'))]));
}
function chooseSearch(index) {
  const item = searchItems[index]; if (!item) return;
  searchBox.value = ''; searchBox.setAttribute('data-open', 'false'); searchActive = -1; renderSearch(); searchBox.blur?.();
  navigate(item.page, item.id);
}
searchBox.addEventListener('input', () => { searchBox.setAttribute('data-open', 'true'); searchActive = searchMatches(searchBox.value ?? '').length ? 0 : -1; renderSearch(); });
searchBox.addEventListener('focus', () => { searchBox.setAttribute('data-open', 'true'); renderSearch(); void load('sessions').then(() => { if (searchBox.getAttribute('data-open') === 'true') renderSearch(); }); });
// A narrow top bar has room for one word in the field.
const searchWidth = globalThis.matchMedia?.('(max-width:720px)');
const nameSearch = () => { searchBox.setAttribute('placeholder', searchWidth?.matches === true ? t('app.34') : t('app.35')); };
searchWidth?.addEventListener?.('change', nameSearch); nameSearch();
searchBox.addEventListener('blur', () => { searchBox.setAttribute('data-open', 'false'); renderSearch(); });
searchBox.addEventListener('keydown', event => {
  if (event.key === 'ArrowDown' || event.key === 'ArrowUp') { event.preventDefault(); if (!searchItems.length) return; searchActive = (searchActive + (event.key === 'ArrowDown' ? 1 : -1) + searchItems.length) % searchItems.length; searchBox.setAttribute('data-open', 'true'); renderSearch(); }
  else if (event.key === 'Enter') { event.preventDefault(); chooseSearch(Math.max(0, searchActive)); }
  else if (event.key === 'Escape') { searchBox.value = ''; searchBox.setAttribute('data-open', 'false'); renderSearch(); }
});
document.addEventListener('keydown', event => {
  if (drawerModal()) {
    if (event.key === 'Escape') { event.preventDefault(); setNav(false, { restoreFocus: true }); return; }
    if (event.key === 'Tab') {
      const controls = drawerControls(), first = controls[0], last = controls.at(-1);
      const active = $('#sidebar').contains(event.target) ? event.target : document.activeElement;
      if (!first) { event.preventDefault(); return; }
      if (!$('#sidebar').contains(active) || event.shiftKey && active === first || !event.shiftKey && active === last) {
        event.preventDefault(); (event.shiftKey ? last : first).focus?.();
      }
    }
    return;
  }
  const typing = event.target?.closest?.('input,textarea,select,[contenteditable],dialog');
  if (event.key === '/' && !typing && !$('#console').hidden && !event.ctrlKey && !event.metaKey && !event.altKey) { event.preventDefault(); ($('.global-search').hidden ? $('#content .query-box input[type=search]') : searchBox)?.focus?.(); }
});

accountSecurityController = createAccountSecurityController({
  document: ownDocument, createView: createAccountSecurityView,
  request: (path, method, data, options) => api(path, method, data, options),
  owner: () => state.user ? sessionOwner() : null,
  isCurrent: owner => !!owner?.user && sessionCurrent(owner),
  origin: () => globalThis.location.origin,
  accountName: () => state.user?.username ?? '',
  onClosed: () => { if (globalThis.document === ownDocument && !state.user) $('#login-form').elements.username.focus?.(); },
  retireAuthentication: (ticket, { uncertain } = {}) => {
    const csrfToken = state.csrf, admittedOrigin = globalThis.location.origin;
    showLogin({ completedMfa: ticket });
    if (uncertain) void retireMfaCookie({ path: '/api/v1/admin/logout', csrfToken, admittedOrigin, origin: () => globalThis.location.origin });
  },
});
$('#account-security').addEventListener('click', ctx.openAccountSecurity);
// 계정 보안 in the sidebar opens the same dialog; the drawer (narrow screens) closes first.
$('#nav-account-security').addEventListener('click', () => { setNav(false); ctx.openAccountSecurity(); });
const authenticationCurrent = ticket => ticket.revision === authRevision && sessionCurrent(ticket.owner);
$('#login-form').addEventListener('submit', async event => {
  let owner = sessionOwner();
  event.preventDefault(); accountSecurityController.retire(); resetAdminMfa();
  const ticket = { owner, revision: authRevision }, form = event.target, submit = form.querySelector('button');
  submit.disabled = true; submit.textContent = t('app.36'); $('#login-error').textContent = ''; $('#login-error').className = 'error';
  const input = Object.fromEntries(new FormData(form));
  try {
    const result = await requestMfaAuthentication({ path: '/api/v1/admin/login', data: input, owner: ticket, isCurrent: authenticationCurrent, origin: () => globalThis.location.origin });
    if (!authenticationCurrent(ticket)) return;
    form.reset();
    if (result.kind === 'challenge') {
      adminMfaChallenge = { ...result.challenge, ticket, deadline: createMfaDeadline(result.challenge.expiresAt) };
      const pending = adminMfaChallenge;
      adminMfaTimer = setTimeout(() => { if (adminMfaChallenge === pending && authenticationCurrent(ticket)) { showLogin(); $('#login-error').textContent = t('common.26'); } }, pending.deadline.remaining());
      $('#login').hidden = true; $('#admin-mfa').hidden = false; $('#admin-mfa-form').elements.code.focus?.();
    } else { const entering = enter(result.session); owner = sessionOwner(); await entering; }
  } catch (error) { if (sessionCurrent(owner) && ticket.revision === authRevision) { $('#login-error').textContent = error.message; form.elements.password.value = ''; form.elements.password.focus?.(); } }
  finally { if (sessionCurrent(owner) && ticket.revision === authRevision) { submit.disabled = false; submit.textContent = t('common.16'); } input.password = ''; }
});
$('#admin-mfa-mode').addEventListener('click', () => {
  if (adminMfaBusy || !adminMfaChallenge) return;
  adminMfaRecovery = !adminMfaRecovery; const form = $('#admin-mfa-form'); form.reset();
  $('#admin-mfa-code').hidden = adminMfaRecovery; $('#admin-mfa-form').elements.code.disabled = adminMfaRecovery;
  $('#admin-mfa-recovery').hidden = !adminMfaRecovery; $('#admin-mfa-form').elements.recoveryCode.disabled = !adminMfaRecovery;
  $('#admin-mfa-mode').textContent = adminMfaRecovery ? t('app.37') : t('app.9');
  form.elements[adminMfaRecovery ? 'recoveryCode' : 'code'].focus?.();
});
$('#admin-mfa-cancel').addEventListener('click', () => showLogin());
$('#admin-mfa-form').addEventListener('submit', async event => {
  event.preventDefault(); const pending = adminMfaChallenge, form = event.target;
  if (!pending || adminMfaBusy || !authenticationCurrent(pending.ticket)) return;
  const value = adminMfaRecovery ? normalizeMfaRecoveryCode(form.elements.recoveryCode.value) : normalizeMfaCode(form.elements.code.value);
  form.reset(); $('#admin-mfa-error').textContent = '';
  if (!pending.deadline.valid()) { showLogin(); $('#login-error').textContent = t('common.26'); return; }
  if (value === null) { $('#admin-mfa-error').textContent = new MfaClientError(400, 'invalid_mfa_fields').message; return; }
  const input = { challengeToken: pending.challengeToken, [adminMfaRecovery ? 'recoveryCode' : 'code']: value };
  adminMfaBusy = true; for (const control of form.querySelectorAll('input,button')) control.disabled = control.id !== 'admin-mfa-cancel';
  try {
    const result = await requestMfaAuthentication({ path: '/api/v1/admin/mfa/verify', data: input, owner: pending.ticket, isCurrent: authenticationCurrent, origin: () => globalThis.location.origin });
    if (!authenticationCurrent(pending.ticket)) return;
    if (result.kind !== 'session') throw new MfaClientError(0, 'mfa_response_invalid', { uncertain: true });
    await enter(result.session);
  } catch (error) {
    if (!authenticationCurrent(pending.ticket)) return;
    if (error.status === 410 || error.status === 401 || error.uncertain) { showLogin(); $('#login-error').textContent = error.message; }
    else $('#admin-mfa-error').textContent = error.message;
  } finally {
    input.challengeToken = ''; input.code = ''; input.recoveryCode = '';
    if (authenticationCurrent(pending.ticket)) { adminMfaBusy = false; for (const control of form.querySelectorAll('input,button')) control.disabled = false; $('#admin-mfa-form').elements.code.disabled = adminMfaRecovery; $('#admin-mfa-form').elements.recoveryCode.disabled = !adminMfaRecovery; }
  }
});
$('#setup-form').addEventListener('submit', async event => {
  const owner = sessionOwner();
  event.preventDefault(); const form = event.target;
  const input = Object.fromEntries(new FormData(form));
  $('#setup-error').textContent = '';
  if (input.password !== input.confirmation) { $('#setup-error').textContent = t('app.38'); form.elements.confirmation.focus?.(); return; }
  const buttons = [...form.querySelectorAll('button')]; buttons.forEach(node => node.disabled = true);
  try {
    await api('/setup', 'POST', { username: input.username, password: input.password, passwordConfirmation: input.confirmation, setupCode: input.setupCode });
    if (!sessionCurrent(owner)) return;
    form.reset(); showLogin();
    $('#login-form').elements.username.value = input.username;
    // A success message, so it uses the success tone rather than the error style of this field.
    $('#login-error').textContent = t('app.39'); $('#login-error').className = 'form-message is-success';
  } catch (error) { if (sessionCurrent(owner)) { $('#setup-error').textContent = error.message; form.elements.password.value = ''; form.elements.confirmation.value = ''; if (/^(setup_code_|too_many_setup)/.test(error.code ?? '')) form.elements.setupCode.focus?.(); } }
  finally { if (sessionCurrent(owner)) buttons.forEach(node => node.disabled = false); input.password = ''; input.confirmation = ''; input.setupCode = ''; }
});
$('#setup-restore-open')?.addEventListener('click', () => { if (state.user?.mustChangeCredentials) showSetupRestore(true); });
$('#setup-logout').addEventListener('click', async () => { const owner = sessionOwner(); accountSecurityController.retire(); resetAdminMfa(); try { await api('/logout', 'POST'); if (!sessionCurrent(owner)) return; $('#setup-form').reset(); showLogin(); } catch (error) { if (sessionCurrent(owner)) $('#setup-error').textContent = error.message; } });
document.querySelectorAll('#sidebar [data-page]').forEach(node => node.addEventListener('click', event => { if (event.ctrlKey || event.metaKey || event.shiftKey) return; event.preventDefault?.(); navigate(node.dataset.page); }));
$('#refresh').addEventListener('click', async () => { const owner = sessionOwner(); await refresh(); if (!sessionCurrent(owner)) return; loadPage(true); void loadServerInfo(); });
$('#server-version-info').addEventListener('click', () => navigate('updates'));
$('#server-update-indicator').addEventListener('click', () => navigate('updates'));
$('#nav-toggle').addEventListener('click', () => setNav(!$('#console').classList.contains('nav-open')));
$('#nav-scrim').addEventListener('click', () => setNav(false, { restoreFocus: true }));
$('#nav-close').addEventListener('click', () => setNav(false, { restoreFocus: true }));
$('.skip-link').addEventListener('click', event => { event.preventDefault(); $('#content').focus?.(); });
$('#logout').addEventListener('click', async () => { const owner = sessionOwner(); accountSecurityController.retire(); resetAdminMfa(); try { await api('/logout', 'POST'); if (sessionCurrent(owner)) showLogin(); } catch (error) { if (sessionCurrent(owner)) notice(error.message, true); } });
$('#modal-close').onclick = $('#modal-cancel').onclick = () => { modalRevision++; $('#modal-form').reset(); $('#modal-fields').replaceChildren(); $('#modal').close(); };
// Back/forward and edited addresses.
const followHash = () => {
  if (!state.user || !state.data || globalThis.document !== ownDocument) return;
  // The first setup has three pages: Back, Forward and a typed address move between them (they were ignored, so the
  // address and the page disagreed); the address of any other page goes back to the one that is shown.
  if (setupRequired()) {
    const wanted = parseHash(globalThis.location.hash), page = firstSetupPages.includes(wanted.page) ? wanted.page : 'proxmox';
    if (page !== state.route.page) { closeDialogs(); navigate(page, null, { push: false }); }
    pushHash(true);
    return;
  }
  const route = parseHash(globalThis.location.hash), moved = route.page !== state.route.page || route.id !== state.route.id, step = stepOf();
  // Back from a window that is a step of its own: the window closes and the page stays.
  const left = !moved && windowStep !== null && step === null && !!shownWindow();
  if (moved || left) closeDialogs();
  followingHistory = true;
  try {
    if (moved) { if (step !== null) windowStep = { page: route.page, id: route.id }; navigate(route.page, route.id, { push: false }); }
    else if (left) { windowStep = null; closeDetail(); }
  } finally { followingHistory = false; }
  if (!shownWindow() || step === null) windowStep = null;
  pushHash(true);
};
globalThis.addEventListener?.('hashchange', followHash);
globalThis.addEventListener?.('popstate', followHash);
globalThis.addEventListener?.('pagehide', () => { if (globalThis.document === ownDocument) { accountSecurityController?.retire(); resetAdminMfa(); } });
// Leaving the document with an unsaved form asks first.
globalThis.addEventListener?.('beforeunload', event => {
  if (globalThis.document !== ownDocument || !state.user || !(state.ui.serverAccess?.dirty === true || modalDraftDirty())) return;
  event.preventDefault(); event.returnValue = '';
});
const refreshServerOnReturn = () => { if (globalThis.document === ownDocument && state.user && ownDocument.visibilityState !== 'hidden') void loadServerInfo(); };
globalThis.addEventListener?.('focus', refreshServerOnReturn);
document.addEventListener('visibilitychange', refreshServerOnReturn);
// The version that served this page, also while it waits on the sign-in screen.
void fetch('/healthz', { credentials: 'omit' }).then(response => response.json()).then(health => { if (health?.service === 'multitap-broker' && /^\d+\.\d+\.\d+$/.test(health.version)) pageVersion ??= health.version; }).catch(() => {});
const initialOwner = sessionOwner();
try {
  const session = await api('/session');
  if (sessionCurrent(initialOwner)) {
    const entering = enter(session), owner = sessionOwner();
    try { await entering; } catch { if (sessionCurrent(owner)) showLogin(); }
  }
} catch { if (sessionCurrent(initialOwner)) showLogin(); }
const clientRelease = await productRelease('client');
// Offered on the sign-in footer; inside the console 다운로드 has every file.
if (clientRelease) for (const node of document.querySelectorAll('[data-client-download]')) { node.href = clientRelease.url; node.title = t('app.40', { version: clientRelease.version }); node.replaceChildren(icon('download'), el('span', { class: 'download-text' }, t('app.41', { version: clientRelease.version }))); node.hidden = false; }
