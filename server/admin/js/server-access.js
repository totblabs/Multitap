// Administrator-only configuration UI. All probing/apply/rollback belongs to
// the authenticated server-access API; the browser never opens a listener.
import { el, button, icon, stateDot, panel, facts, disable, loadingState } from './dom.js';
import { announceFirst, announcements, everyoneAudience } from './notices.js';
import { t } from './i18n.js';

const protocol = 'server-access-v1';
const pendingStates = new Set(['waiting_idle', 'applying', 'health_check']);
const operationStates = new Set([...pendingStates, 'applied', 'failed_rolled_back']);
const clone = value => JSON.parse(JSON.stringify(value));
const revision = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
const bounded = (value, max = 2048) => typeof value === 'string' && value.length <= max;
const now = ctx => ctx.now?.() ?? Date.now();
const expires = value => typeof value === 'number' ? value : Date.parse(value);
const stable = value => JSON.stringify(value, (_key, item) => item && typeof item === 'object' && !Array.isArray(item)
  ? Object.fromEntries(Object.keys(item).sort().map(key => [key, item[key]])) : item);
const same = (a, b) => stable(a) === stable(b);

function origin(value, scheme) {
  if (!bounded(value) || !value || value !== value.trim()) return null;
  try {
    const url = new URL(value);
    return url.protocol === scheme + ':' && !url.username && !url.password &&
      !url.search && !url.hash && url.pathname === '/' && !value.includes('?') && !value.includes('#')
      ? url : null;
  } catch { return null; }
}
// An address typed without a scheme takes its card's (otherwise a bare
// "10.0.0.5:18480" is not recognised and the internal port does not follow).
export const withScheme = (value, kind) => { const text = String(value ?? '').trim(); return text && !text.includes('://') ? kind + '://' + text : text; };
const canonical = (value, kind) => { const text = withScheme(value, kind); return origin(text, kind)?.origin ?? text; };
// access-origins-v1: up to three more exact addresses per card on its listener.
const maxExtra = 3;
const listOf = row => row.origin ? [row.origin, ...(row.extraOrigins ?? [])] : [];
function exactIp(value) {
  if (typeof value !== 'string' || !value || value !== value.trim() || /[%/\s]/.test(value)) return false;
  if (/^(?:0|[1-9]\d{0,2})(?:\.(?:0|[1-9]\d{0,2})){3}$/.test(value)) return value.split('.').every(part => Number(part) <= 255);
  try { return value.includes(':') && new URL('http://[' + value + ']/').hostname.startsWith('['); } catch { return false; }
}
function configShape(config) {
  if (!config || config.protocol !== protocol || !bounded(config.primaryOrigin) || !Array.isArray(config.trustedProxies) || config.trustedProxies.some(ip => !bounded(ip, 64))) return false;
  return ['https', 'http'].every(kind => {
    const value = config[kind];
    return value && typeof value.enabled === 'boolean' && (value.origin === null || bounded(value.origin)) &&
      (value.extraOrigins === undefined || Array.isArray(value.extraOrigins) && value.extraOrigins.length <= maxExtra && value.extraOrigins.every(item => bounded(item))) &&
      bounded(value.listenHost, 255) && Number.isInteger(value.listenPort) && value.listenPort >= 1 && value.listenPort <= 65535 &&
      (kind !== 'https' || ['proxy', 'direct'].includes(value.mode) &&
        (value.certFile == null || bounded(value.certFile, 4096)) && (value.keyFile == null || bounded(value.keyFile, 4096)));
  });
}
function normalized(value) {
  if (!value || typeof value !== 'object') return null;
  const applied = Object.hasOwn(value, 'applied') ? value.applied : value.config;
  const state = value.state ?? value.runtime?.state;
  const aliases = { succeeded: 'applied', rolled_back: 'failed_rolled_back' };
  return { ...value, applied, state: aliases[state] ?? state,
    applySupported: value.supported === false || value.applySupported === false ? false : value.applySupported ?? value.supported,
    operationId: value.operationId ?? value.runtime?.operationId,
    errorCode: value.errorCode ?? value.runtime?.errorCode,
    pending: Object.hasOwn(value, 'pending') ? value.pending : value.pendingConfig
      ? { config: value.pendingConfig, state: aliases[state] ?? state, operationId: value.operationId, errorCode: value.errorCode } : null };
}
export function validResponse(raw) {
  const value = normalized(raw);
  return value?.protocol === protocol && (revision(value.revision) || value.revision === 'bootstrap' && value.applied === null) && (value.applied === null || configShape(value.applied)) &&
    (value.pending === null || value.pending && configShape(value.pending.config) && typeof value.pending.state === 'string');
}
function draftOf(config, listener) {
  if (!config) {
    // What the server reports of the listener this request came in on (src/http.mjs serverInfo.listener):
    // { address, port }. An IPv4 address seen through a dual-stack socket comes as ::ffff:a.b.c.d.
    const address = typeof listener?.address === 'string' ? listener.address.replace(/^::ffff:(?=\d{1,3}(?:\.\d{1,3}){3}$)/i, '') : '';
    const observed = !!address && bounded(address, 255) && Number.isInteger(listener?.port) && listener.port >= 1 && listener.port <= 65535;
    const listenHost = observed ? address : '', listenPort = observed ? String(listener.port) : '';
    return { primaryOrigin: '', https: { enabled: true, origin: '', extraOrigins: [], mode: 'proxy', listenHost, listenPort, certFile: '', keyFile: '' },
      http: { enabled: false, origin: '', extraOrigins: [], listenHost, listenPort }, trustedProxies: '' };
  }
  return { primaryOrigin: config.primaryOrigin, https: {
    enabled: config.https.enabled, origin: config.https.origin ?? '', extraOrigins: [...config.https.extraOrigins ?? []], mode: config.https.mode,
    listenHost: config.https.listenHost, listenPort: String(config.https.listenPort),
    certFile: config.https.certFile ?? '', keyFile: config.https.keyFile ?? '',
  }, http: { enabled: config.http.enabled, origin: config.http.origin ?? '', extraOrigins: [...config.http.extraOrigins ?? []],
    listenHost: config.http.listenHost, listenPort: String(config.http.listenPort) },
  trustedProxies: config.trustedProxies.join('\n') };
}
// Only these canonical fields cross the API. NAT help inputs stay local.
export function draftConfig(draft) {
  const primaryText = draft.primaryOrigin.trim();
  const primary = (origin(primaryText, 'https') ?? origin(primaryText, 'http'))?.origin ?? primaryText;
  const endpoint = kind => {
    const value = draft[kind], list = [canonical(value.origin, kind), ...(value.extraOrigins ?? []).map(item => canonical(item, kind)).filter(Boolean)];
    // The chosen 기본 서버 주소 leads its card's list: the policy's main address.
    const at = list.indexOf(primary); if (at > 0) list.unshift(...list.splice(at, 1));
    return { enabled: value.enabled, origin: value.enabled ? list[0] : null, ...(value.enabled && list.length > 1 ? { extraOrigins: list.slice(1) } : {}),
      listenHost: value.listenHost.trim(), listenPort: /^\d+$/.test(value.listenPort) ? Number(value.listenPort) : null };
  };
  return { protocol, primaryOrigin: primary, https: { ...endpoint('https'), mode: draft.https.mode,
    certFile: draft.https.enabled && draft.https.mode === 'direct' ? draft.https.certFile.trim() || null : null,
    keyFile: draft.https.enabled && draft.https.mode === 'direct' ? draft.https.keyFile.trim() || null : null },
  http: endpoint('http'), trustedProxies: draft.trustedProxies.split(/\r?\n/).map(ip => ip.trim()).filter(Boolean) };
}
function validation(config) {
  const errors = [];
  if (!config.https.enabled && !config.http.enabled) errors.push(t('server-access.1'));
  for (const kind of ['https', 'http']) {
    const value = config[kind], list = value.enabled ? [value.origin, ...value.extraOrigins ?? []] : [];
    if (list.some(item => !origin(item, kind))) errors.push(t('server-access.2', { toUpperCase: kind.toUpperCase() }));
    else if (new Set(list).size !== list.length) errors.push(t('server-access.3', { toUpperCase: kind.toUpperCase() }));
    if (!value.listenHost || /[\s/]/.test(value.listenHost)) errors.push(t('server-access.4', { toUpperCase: kind.toUpperCase() }));
    if (!Number.isInteger(value.listenPort) || value.listenPort < 1 || value.listenPort > 65535) errors.push(t('server-access.5', { toUpperCase: kind.toUpperCase() }));
  }
  const enabledOrigins = ['https', 'http'].filter(kind => config[kind].enabled).flatMap(kind => listOf(config[kind]));
  if (!enabledOrigins.includes(config.primaryOrigin)) errors.push(t('server-access.6'));
  if (config.https.enabled && config.https.mode === 'direct' && (!config.https.certFile || !config.https.keyFile)) errors.push(t('server-access.7'));
  if (config.trustedProxies.some(ip => !exactIp(ip)) || new Set(config.trustedProxies).size !== config.trustedProxies.length) errors.push(t('server-access.8'));
  return errors;
}
// HTTP has no hop of its own: it listens on its external address's port and
// shares HTTPS's proxy listener when the ports are the same. A listen value
// the administrator typed in this edit is left alone.
export function followListener(draft) {
  const http = draft.http, url = origin(withScheme(http.origin, 'http'), 'http');
  if (http.listenEdited || !url) return false;
  http.listenPort = url.port || '80'; http.directHost ??= http.listenHost;
  http.listenHost = draft.https.enabled && draft.https.mode === 'proxy' && draft.https.listenPort === http.listenPort ? draft.https.listenHost : http.directHost;
  return true;
}
const externalPort = value => { const url = origin(value ?? '', 'http') ?? origin(value ?? '', 'https'); return url ? url.port || (url.protocol === 'https:' ? '443' : '80') : '?'; };
const errorText = (error, config = null) => ({
  server_access_external_port_conflict: config && [config.http, config.https].filter(row => row.enabled && row.mode !== 'proxy' && String(row.listenPort) !== externalPort(row.origin))
    .map(row => t('server-access.9', { externalPort: externalPort(row.origin), listenPort: row.listenPort }))[0] || t('server-access.10'),
  server_access_udp_port_conflict: t('server-access.11'),
  server_access_listener_conflict: t('server-access.12'),
  server_access_listener_busy: t('server-access.13'),
  // The codes the updater really answers (update/server-access-engine.mjs, server-access-deployment.mjs,
  // server-access-policy.mjs), passed on by update/agent.mjs and src/server-access-client.mjs.
  server_access_revision_changed: t('server-access.14'),
  server_access_test_required: t('server-access.15'),
  server_access_test_failed: t('server-access.163'),
  server_access_busy: t('server-access.17'),
  server_access_deployment_unsupported: t('server-access.18'),
  server_access_recovery_required: t('server-access.56'),
  server_access_tls_not_readable: t('server-access.tlsNotReadableF2'),
  server_access_tls_identity_invalid: t('server-access.tlsIdentityInvalidF2'),
  server_access_tls_path_invalid: t('server-access.tlsPathInvalidF2'),
  server_access_tls_configuration_invalid: t('server-access.7'),
  server_access_privileged_listener_not_configured: t('server-access.lowPortF2'),
  server_access_listen_host_invalid: t('server-access.listenHostInvalidF2'),
  server_access_listener_host_unavailable: t('server-access.listenHostUnavailableF2'),
  server_access_listener_unverified: t('server-access.listenerUnverifiedF2'),
  server_access_listener_privilege_unverified: t('server-access.listenerUnverifiedF2'),
  server_access_trusted_proxies_invalid: t('server-access.8'),
  server_access_last_management_path: t('server-access.1'),
  server_access_primary_origin_invalid: t('server-access.6'),
  server_access_origin_invalid: t('server-access.19'),
  // The browser's address is not one of the saved ones.
  origin_not_allowed: t('server-access.20'),
  server_access_management_origin_changed: t('server-access.21'),
  server_access_cancel_too_late: t('server-access.22'),
  server_access_cancel_unavailable: t('server-access.23'),
}[error?.code] ?? error?.message ?? t('server-access.24'));

// What a saved change is waiting for, in plain words. Only the server's own
// unfinished work holds it (update/server-access-engine.mjs); connections are
// ended, not waited for.
const blockerSubjects = { server_access_host_updates_busy: t('server-access.25'), server_access_desktop_settings_busy: t('server-access.26'), server_access_power_busy: t('server-access.27') };
function blockersOf(snapshot) {
  const list = snapshot.blockers;
  return Array.isArray(list) && list.length <= 8 && list.every(row => row && bounded(row.code, 100) && Number.isInteger(row.count) && row.count >= 0 && row.count <= 100000) ? list : [];
}
export function blockerLines(blockers, timedOut = false) {
  return blockers.map(row => blockerSubjects[row.code] && row.count > 0
    ? (timedOut ? t('server-access.28', { count: row.count, value: blockerSubjects[row.code] }) : t('server-access.29', { count: row.count, value: blockerSubjects[row.code] }))
    : timedOut ? t('server-access.30') : t('server-access.31'));
}
const clock = value => { const date = new Date(value); return Number.isFinite(date.getTime()) ? String(date.getHours()).padStart(2, '0') + ':' + String(date.getMinutes()).padStart(2, '0') : null; };
// The one confirmation before a save: what happens, in two or three lines.
export function saveDescription(config, snapshot) {
  const people = snapshot?.connections?.people;
  return (Number.isInteger(people) && people > 0 ? t('server-access.32', { people })
    : Number.isInteger(people) ? t('server-access.33') : t('server-access.34')) +
    '\n' + (config.http.enabled ? t('server-access.35') : t('server-access.36')) +
    t('server-access.37');
}

function stateFor(ctx, snapshot) {
  let model = ctx.state.ui.serverAccess;
  if (!model || model.owner !== ctx.state.user) {
    model = { owner: ctx.state.user, baseRevision: snapshot.revision, applied: clone(snapshot.applied),
      draft: draftOf(snapshot.applied, snapshot.bootstrapListener), edits: 0, dirty: false, conflict: false, receipt: null,
      lastTest: null, testing: false, saving: false, cancelling: false, operation: null, error: '', validation: [],
      assist: { targetIp: '', proxyPort: '' }, expiryTimer: null };
    ctx.state.ui.serverAccess = model;
  }
  const appliedChanged = snapshot.revision !== model.baseRevision;
  const matchingOperation = model.operation?.operationId && model.operation.operationId === snapshot.operationId;
  // The result of this page's own save. The server stores its own spelling of what was typed (localhost →
  // 127.0.0.1, lower-case IPv6: update/server-access-policy.mjs), so the applied settings are taken as they are.
  if (matchingOperation && snapshot.state === 'applied' && !snapshot.pending && model.operation.state !== 'applied' && (appliedChanged || !model.dirty)) {
    model.operation = { ...model.operation, state: 'applied' };
    model.draft = draftOf(snapshot.applied); model.dirty = false; model.conflict = false;
  }
  if (appliedChanged) {
    clearTimeout(model.expiryTimer); model.receipt = null;
    if (model.dirty && model.operation?.state !== 'applied') model.conflict = true;
    else { model.draft = draftOf(snapshot.applied, snapshot.bootstrapListener); model.baseRevision = snapshot.revision; model.applied = clone(snapshot.applied); model.lastTest = null; }
  }
  if (matchingOperation && snapshot.state === 'failed_rolled_back') model.operation = { ...model.operation, state: 'failed_rolled_back', errorCode: snapshot.errorCode };
  // Cancelled or timed out: the operation is over and the form opens again
  // (it stayed locked until a reload). An unverified failure keeps `pending`.
  if (matchingOperation && snapshot.state === 'failed') model.operation = { ...model.operation, state: 'failed', errorCode: snapshot.errorCode };
  // A save that did not happen leaves the form's edits unsaved again (saveDraft cleared the mark when it was accepted).
  if (matchingOperation && ['failed_rolled_back', 'failed'].includes(snapshot.state)) model.dirty = !same(draftConfig(model.draft), model.applied);
  else if (model.operation?.state === 'failed' && snapshot.operationId && !matchingOperation) model.operation = null;
  return model;
}
const current = (ctx, model) => ctx.state.user === model.owner && ctx.state.ui.serverAccess === model;
const locked = (model, snapshot) => snapshot.applySupported === false || model.saving || pendingStates.has(model.operation?.state) || !!snapshot.pending && snapshot.pending.state !== 'failed_rolled_back';
function invalidate(model) {
  model.edits++; model.dirty = !same(draftConfig(model.draft), model.applied); model.receipt = null; model.lastTest = null;
  model.error = ''; model.validation = []; clearTimeout(model.expiryTimer);
}
function testCurrent(ctx, model, snapshot) {
  return !!model.receipt && model.receipt.revision === snapshot.revision && !model.conflict &&
    model.receipt.expires > now(ctx) && model.receipt.fingerprint === JSON.stringify(draftConfig(model.draft));
}
async function testDraft(ctx, model, snapshot) {
  if (!current(ctx, model) || model.testing || locked(model, snapshot) || model.conflict) return;
  const config = draftConfig(model.draft); model.validation = validation(config);
  if (model.validation.length) { ctx.render(); return; }
  const edits = model.edits, owner = model.owner, baseRevision = model.baseRevision;
  model.testing = true; model.receipt = null; model.error = ''; ctx.render();
  try {
    const result = await ctx.api('/server-access/test', 'POST', { config, baseRevision });
    if (!current(ctx, model) || ctx.state.user !== owner || model.edits !== edits || ctx.value('serverAccess')?.revision !== baseRevision) return;
    const modern = result?.scope === 'preflight' && result.baseRevision === baseRevision && revision(result.configHash) && result.requiresRestart === true;
    const legacy = result?.revision === baseRevision && typeof result.ok === 'boolean';
    if ((!modern && !legacy) || !Array.isArray(result.checks) || result.checks.some(check =>
      !bounded(modern ? check.name : check.id, 100) || (check.code != null && (!bounded(check.code, 100) || !/^[a-z0-9_.-]+$/i.test(check.code))) ||
      (modern ? !['passed', 'pending', 'failed'].includes(check.status) : typeof check.ok !== 'boolean'))) throw Error(t('server-access.38'));
    const checks = result.checks.map(check => modern ? { id: check.name, status: check.status, ok: check.status === 'passed', code: check.code } : { ...check, status: check.ok ? 'passed' : 'failed' });
    const admitted = modern ? checks.length > 0 && checks.every(check => check.status !== 'failed') : result.ok;
    model.lastTest = { ...result, checks, ok: admitted, scope: 'preflight' };
    if (admitted) {
      const until = expires(result.expiresAt);
      if (!bounded(result.testId, 200) || !result.testId || !Number.isFinite(until) || until <= now(ctx) || !checks.length || checks.some(check => check.status === 'failed')) throw Error(t('server-access.38'));
      model.receipt = { testId: result.testId, revision: baseRevision, expires: until, fingerprint: JSON.stringify(config) };
      model.expiryTimer = setTimeout(() => { if (current(ctx, model)) { model.receipt = null; ctx.render(); } }, Math.min(2147483647, until - now(ctx) + 1));
      model.expiryTimer.unref?.();
    }
  } catch (error) { if (current(ctx, model) && model.edits === edits) model.error = errorText(error, config); }
  finally { if (current(ctx, model)) { model.testing = false; ctx.render(); } }
}
function saveDraft(ctx, model, snapshot) {
  if (!current(ctx, model) || !model.dirty || locked(model, snapshot) || !testCurrent(ctx, model, snapshot)) return;
  const edits = model.edits, config = draftConfig(model.draft), receipt = model.receipt;
  const description = saveDescription(config, snapshot);
  // 먼저 알리기 (js/notices.js): a save ends every connection.
  const announce = announceFirst(ctx, { audience: everyoneAudience(ctx), text: announcements.serverAccess });
  ctx.modal(t('server-access.39'), announce ? [{ node: announce.node }] : [], (announce ? announce.wrap : run => run)(async () => {
    const latest = normalized(ctx.value('serverAccess'));
    if (!current(ctx, model) || !model.dirty || model.edits !== edits || !validResponse(latest) || !testCurrent(ctx, model, latest) || locked(model, latest)) throw Error(t('server-access.40'));
    model.saving = true; model.error = ''; model.receipt = null; clearTimeout(model.expiryTimer); ctx.render();
    try {
      const result = await ctx.api('/server-access/save', 'POST', { config, testId: receipt.testId, baseRevision: receipt.revision, confirm: true });
      if (!current(ctx, model)) return;
      if (!result || !operationStates.has(result.state) || !bounded(result.operationId, 128) || !result.operationId) throw Error(t('server-access.41'));
      // `origins`: the addresses that will answer once it is applied (the browser's own may not be one of them).
      model.operation = { operationId: result.operationId, state: result.state === 'applied' ? 'applying' : result.state, errorCode: result.errorCode,
        primaryOrigin: config.primaryOrigin, origins: ['https', 'http'].filter(kind => config[kind].enabled).flatMap(kind => listOf(config[kind])) };
      // The edits are with the server now: leaving the page no longer asks about unsaved changes (app.js).
      model.receipt = null; model.dirty = false; clearTimeout(model.expiryTimer);
      void ctx.load('serverAccess', { force: true });
    } catch (error) { if (current(ctx, model)) model.error = errorText(error, config); throw error; }
    finally { if (current(ctx, model)) { model.saving = false; ctx.render(); } }
  }), { save: t('server-access.42'), description, reload: false, danger: config.http.enabled });
}
// Withdraws a save that is still waiting. The server answers once it has
// confirmed nothing changed; the next status read opens the form again.
async function cancelPending(ctx, model, operationId) {
  if (!current(ctx, model) || model.cancelling || !bounded(operationId, 128) || !operationId) return;
  model.cancelling = true; model.error = ''; ctx.render();
  try { await ctx.api('/server-access/cancel', 'POST', { operationId }); }
  catch (error) { if (current(ctx, model)) model.error = errorText(error); }
  finally {
    if (current(ctx, model)) { model.cancelling = false; ctx.render(); void ctx.load('serverAccess', { force: true }); }
  }
}

function statusText(model, snapshot) {
  if (snapshot.state === 'recovery_required') return snapshot.currentObserved === false
    ? t('server-access.43')
    : t('server-access.44');
  const state = snapshot.pending?.state ?? model.operation?.state ?? snapshot.state;
  if (state === 'waiting_idle') return blockersOf(snapshot).length ? t('server-access.45') : t('server-access.46');
  // Ended before anything was written, and the server confirmed that.
  if (state === 'failed' && snapshot.state === 'failed' && snapshot.unchangedVerified === true && !snapshot.pending) {
    if (snapshot.errorCode === 'server_access_cancelled') return t('server-access.47');
    if (snapshot.errorCode === 'server_access_idle_timeout') return [...(blockersOf(snapshot).length ? blockerLines(blockersOf(snapshot), true) : [t('server-access.48')]), t('server-access.49')].join('\n');
    return t('server-access.50');
  }
  if (state === 'applying') return t('server-access.51');
  if (state === 'health_check') return t('server-access.52');
  if (state === 'failed_rolled_back') return (snapshot.pending?.errorCode ?? model.operation?.errorCode ?? snapshot.errorCode) === 'server_access_health_failed'
    ? t('server-access.53')
    : t('server-access.54');
  // Said after a save made in this visit only: what the server applied before is simply the current setting.
  if (state === 'applied' && model.operation?.state === 'applied') return t('server-access.55');
  if (state === 'failed' || state === 'recovery_required') return t('server-access.56');
  if (snapshot.pending) return t('server-access.57');
  if (snapshot.applied === null) return t('server-access.58');
  return model.dirty ? t('server-access.59') : '';
}
const portOf = (value, kind) => { const url = origin(withScheme(value, kind), kind); return url ? url.port || (kind === 'https' ? '443' : '80') : t('server-access.61'); };
function dockerMapping(ports, kind, containerPort) {
  const matches = Array.isArray(ports) ? ports.filter(row => row?.protocol === 'tcp' && row.scheme === kind &&
    row.containerPort === containerPort && Number.isInteger(row.hostPort) && row.hostPort >= 1 && row.hostPort <= 65535) : [];
  return matches.length === 1 ? matches[0] : null;
}
export function forwardingRows(config, assist, { dockerPorts = [], plannedDockerPorts = [] } = {}) {
  const rows = [];
  for (const kind of ['https', 'http']) {
    const value = config[kind]; if (!value.enabled) continue;
    const target = exactIp(assist.targetIp.trim()) ? assist.targetIp.trim() : null;
    const proxy = kind === 'https' && value.mode === 'proxy';
    const actual = dockerMapping(dockerPorts, kind, value.listenPort), planned = dockerMapping(plannedDockerPorts, kind, value.listenPort);
    const mapping = actual ?? planned;
    const targetPort = proxy ? /^\d+$/.test(assist.proxyPort) && Number(assist.proxyPort) >= 1 && Number(assist.proxyPort) <= 65535 ? assist.proxyPort : null : mapping?.hostPort ?? value.listenPort;
    const destination = !target ? t('server-access.62') : !targetPort ? (proxy ? t('server-access.63') : t('server-access.64'))
      : (target.includes(':') ? '[' + target + ']' : target) + ':' + targetPort;
    // One row per distinct external port among the card's addresses.
    for (const external of new Set([value.origin, ...value.extraOrigins ?? []].map(item => portOf(item, kind))))
      rows.push({ kind: kind.toUpperCase(), external: String(external), destination, proxy,
        mapping: mapping ? (actual ? t('server-access.65') : t('server-access.66')) + t('server-access.67') + value.listenPort : null });
  }
  return rows;
}
// An address field is plain text (inputmode url): a bare "10.0.0.5:18480"
// is valid here and gets its scheme when the field is left (`settle`).
function addressField(label, name, value, edit, { disabled = false, type = 'text', placeholder = '', hint = null, max = 2048, settle = null } = {}) {
  const id = 'access-' + name.replaceAll('.', '-');
  return el('label', { class: 'server-access-field', for: id }, el('span', {}, label),
    el('input', { id, name, type: type === 'url' ? 'text' : type, inputmode: type === 'url' ? 'url' : null, value: value ?? '', disabled, maxlength: max, autocomplete: 'off', spellcheck: 'false',
      placeholder, 'data-focus': id, oninput: event => edit(event.target.value), onchange: settle ? event => settle(event.target) : null, ...(type === 'number' ? { min: 1, max: 65535 } : {}) }),
    hint ? el('small', { class: 'field-hint' }, hint) : null);
}
// 자세히 · 집 밖에서 접속하려면: the real ports when the page knows them.
// Two ways in from outside, the safe one first, each with its own numbered steps.
function outsideSteps({ httpPort = null, udpPort = null, compose = false } = {}) {
  const tcp = httpPort ? t('server-access.68', { httpPort }) : t('server-access.69'), port = httpPort ?? t('server-access.70');
  const way = (badge, title, ...rows) => el('li', { class: 'outside-way' }, el('strong', {}, title, ' ', el('span', { class: 'plain-tag' }, t('server-access.tag', { tag: badge }))), ...rows);
  const step = (number, text, example = null) => el('div', { class: 'outside-step' }, el('span', { class: 'outside-step-number', 'aria-hidden': 'true' }, String(number)), el('p', {}, text, example ? el('code', {}, ...[example].flat()) : null));
  return [
    way(t('server-access.71'), t('server-access.72'),
      step(1, t('server-access.73')),
      step(2, t('server-access.74'), 'https://vdi.example.com')),
    way(t('server-access.75'), t('server-access.76'),
      step(1, t('server-access.77', { tcp })),
      step(2, (compose ? t('server-access.78') : t('server-access.79')), ['http://', el('span', { class: 'ph' }, t('server-access.80')), ':' + port]),
      step(3, t('server-access.81'), ['http://', el('span', { class: 'ph' }, t('common.60')), '.iptime.org:' + port])),
    // Both ways: the media path comes to the server itself, past any TCP proxy.
    udpPort ? el('li', { class: 'outside-way outside-common' }, el('strong', {}, t('server-access.82'), t('server-access.83')),
      step('+', t('server-access.84', { udpPort })),
      step('·', t('server-access.85'))) : null,
  // Only nodes: the callers hand the list to the browser's own replaceChildren, which writes null as text.
  ].filter(Boolean);
}
function outsideHelp(ports) {
  return el('details', { class: 'server-access-help panel', 'data-keep-open': 'access-outside-help', 'data-access-outside-help': '' },
    el('summary', {}, t('server-access.86')),
    el('ol', { class: 'server-access-help-steps outside-ways', 'data-access-outside-steps': '' }, outsideSteps(ports)),
    el('p', { class: 'server-access-warning' }, t('server-access.87')));
}
const mediaPort = snapshot => Number.isInteger(snapshot.udpPort) && snapshot.udpPort >= 1 && snapshot.udpPort <= 65535 ? snapshot.udpPort : null;
function endpointCard(ctx, model, snapshot, kind, edit, redraw, disabled, { editExtra, settle }) {
  const draft = model.draft[kind], secure = kind === 'https', placeholder = secure ? t('server-access.88') : t('server-access.89');
  draft.extraOrigins ??= [];
  const extraRow = (value, index) => el('div', { class: 'server-access-extra', 'data-access-extra': kind + '.' + index },
    addressField(t('server-access.90') + (index + 1), kind + '.extra.' + index, value, text => editExtra(kind, index, text),
      { disabled, type: 'url', placeholder, settle: input => settle(kind, index, input) }),
    el('small', { class: 'field-hint', 'data-access-external': kind + '.' + index }, t('server-access.91') + portOf(value, kind)),
    button(t('common.147'), () => { if (disabled || !current(ctx, model)) return; model.draft[kind].extraOrigins.splice(index, 1); invalidate(model); redraw(); },
      'quiet', 'trash', { disabled, 'aria-label': t('server-access.92', { value: index + 1 }), 'data-access-extra-remove': kind + '.' + index }));
  const add = () => { const list = model.draft[kind].extraOrigins; if (disabled || !current(ctx, model) || list.length >= maxExtra) return; list.push(''); redraw(); };
  const endpoint = el('fieldset', { class: 'server-access-endpoint-fields', disabled },
    draft.enabled ? addressField(t('server-access.93'), kind + '.origin', draft.origin, value => edit(kind, 'origin', value),
      { disabled, type: 'url', placeholder, hint: t('server-access.94'), settle: input => settle(kind, null, input) }) : null,
    draft.enabled ? el('p', { class: 'field-hint', 'data-access-external': kind }, t('server-access.91') + portOf(draft.origin, kind)) : null,
    draft.enabled ? draft.extraOrigins.map(extraRow) : null,
    draft.enabled ? el('div', { class: 'server-access-extra-add' },
      disable(button(t('server-access.95'), add, 'quiet', 'plus', { 'data-access-extra-add': kind }), disabled ? t('server-access.96') : draft.extraOrigins.length >= maxExtra ? t('server-access.97', { value: maxExtra + 1 }) : null),
      el('small', { class: 'field-hint' }, secure ? t('server-access.98') : t('server-access.99'))) : null,
    secure || !draft.enabled ? null : el('p', { class: 'server-access-warning' }, t('server-access.100')),
    el('details', { class: 'server-access-advanced', 'data-keep-open': 'access-' + kind },
      el('summary', {}, !draft.enabled ? t('server-access.101') : secure ? t('server-access.102') : t('server-access.103')),
      secure ? el('label', { class: 'server-access-field' }, el('span', {}, t('server-access.104')),
        el('select', { name: 'https.mode', 'data-focus': 'access-https-mode', disabled,
          onchange: event => { edit('https', 'mode', event.target.value); redraw(); } },
        el('option', { value: 'proxy', selected: draft.mode === 'proxy' }, t('server-access.105')),
        el('option', { value: 'direct', selected: draft.mode === 'direct' }, t('server-access.106')))) : null,
      secure || !draft.enabled ? null : el('p', { class: 'field-hint' }, t('server-access.107')),
      el('div', { class: 'server-access-bind' },
        addressField(t('server-access.108'), kind + '.listenHost', draft.listenHost, value => edit(kind, 'listenHost', value), { disabled, max: 255 }),
        addressField(t('server-access.109'), kind + '.listenPort', draft.listenPort, value => edit(kind, 'listenPort', value), { disabled, type: 'number', max: 5 })),
      secure && draft.enabled && draft.mode === 'direct' ? el('div', { class: 'server-access-certificates' },
        addressField(t('server-access.110'), 'https.certFile', draft.certFile, value => edit('https', 'certFile', value), { disabled, max: 4096 }),
        addressField(t('server-access.111'), 'https.keyFile', draft.keyFile, value => edit('https', 'keyFile', value), { disabled, max: 4096 }),
        el('p', { class: 'field-hint' }, t('server-access.112'))) : null));
  return el('section', { class: 'server-access-endpoint' + (!draft.enabled ? ' is-off' : ''), 'data-access-endpoint': kind },
    el('div', { class: 'server-access-endpoint-head' }, el('div', { class: 'server-access-endpoint-title' },
      el('h2', {}, kind.toUpperCase() + t('server-access.113')), el('span', { class: 'plain-tag' }, t('server-access.tag', { tag: secure ? t('server-access.71') : t('server-access.114') }))),
      el('label', { class: 'server-access-switch' }, el('input', { type: 'checkbox', role: 'switch', name: kind + '.enabled',
        checked: draft.enabled, disabled, 'aria-label': t('server-access.115', { toUpperCase: kind.toUpperCase() }), 'data-focus': 'access-' + kind + '-enabled',
        onchange: event => { edit(kind, 'enabled', event.target.checked); redraw(); } }),
      el('span', { 'aria-hidden': 'true' }, draft.enabled ? t('server-access.116') : t('server-access.117')))), endpoint);
}

// Docker Compose has no root updater to apply a policy: the page writes the
// .env lines and the operator runs `docker compose up -d` (docs/docker-deployment.md).
const loopbackProxies = list => list.length === 2 && list.includes('127.0.0.1') && list.includes('::1');
// MULTITAP_HTTP_ORIGIN takes a comma-separated list (config.mjs).
export function composeLines({ https, http }, applied = null) {
  const secureText = withScheme(https, 'https'), plainTexts = String(http ?? '').split(',').map(item => withScheme(item, 'http')).filter(Boolean), errors = [];
  const secure = secureText ? origin(secureText, 'https') : null, plain = plainTexts.map(item => origin(item, 'http'));
  if (secureText && !secure) errors.push(t('server-access.118'));
  if (plain.some(url => !url)) errors.push(t('server-access.119'));
  else if (plain.length > maxExtra + 1 || new Set(plain.map(url => url.origin)).size !== plain.length) errors.push(t('server-access.120', { value: maxExtra + 1 }));
  if (!secureText && !plainTexts.length) errors.push(t('server-access.121'));
  if (errors.length) return { lines: [], errors, command: '' };
  // The same change as one command for an install made with install.sh (deploy/docker/multitap).
  const command = 'multitap address ' + [secure?.origin, ...plain.map(url => url.origin)].filter(Boolean).join(' ');
  const lines = ['MULTITAP_PUBLIC_ORIGIN=' + (secure?.origin ?? ''), 'MULTITAP_HTTP_ORIGIN=' + plain.map(url => url.origin).join(','), 'MULTITAP_ALLOW_INSECURE_HTTP=' + (plain.length ? '1' : '0')];
  // HTTP clients reach the published port directly: the first address's port
  // on every IPv4 interface. Docker's IPv6 proxy hop would arrive from the
  // network gateway, the address a same-host HTTPS proxy is trusted as.
  if (plain.length) lines.push('MULTITAP_PUBLISH_PORT=0.0.0.0:' + (plain[0].port || '80'));
  const proxies = applied?.trustedProxies ?? [];
  if (secure && proxies.length && !loopbackProxies(proxies)) lines.push('MULTITAP_TRUSTED_PROXIES=' + proxies.join(','));
  return { lines, errors, command };
}
function composeView(ctx, snapshot) {
  let model = ctx.state.ui.serverAccessCompose;
  if (!model || model.owner !== ctx.state.user || model.revision !== snapshot.revision) {
    const applied = snapshot.applied, management = origin(snapshot.managementOrigin ?? '', 'http');
    model = { owner: ctx.state.user, revision: snapshot.revision, https: applied?.https.enabled ? applied.https.origin : '',
      http: applied?.http.enabled ? listOf(applied.http).join(',') : applied?.https.enabled || !management ? '' : management.origin, note: '' };
    ctx.state.ui.serverAccessCompose = model;
  }
  const output = el('textarea', { name: 'composeEnv', rows: 5, readonly: true, spellcheck: 'false', 'data-access-env': '' });
  const commandOutput = el('code', { class: 'access-command', 'data-access-command': '' });
  // The command only when it would change something, not for the addresses already in use.
  const appliedCommand = snapshot.applied ? composeLines({ https: snapshot.applied.https.enabled ? snapshot.applied.https.origin : '', http: snapshot.applied.http.enabled ? listOf(snapshot.applied.http).join(',') : '' }, snapshot.applied).command : null;
  const sameNote = el('p', { class: 'field-hint', role: 'status', 'data-access-command-same': '' }, t('server-access.122'));
  const errors = el('p', { class: 'is-error', role: 'alert', 'data-access-env-error': '' });
  const warning = el('p', { class: 'server-access-warning', 'data-access-http-warning': '' }, t('server-access.100'));
  const proxyHint = el('p', { class: 'field-hint', 'data-access-proxy-hint': '' }, t('server-access.123'));
  const note = el('p', { class: 'field-hint', role: 'status', 'data-access-copy-status': '' }, model.note);
  const update = () => {
    const result = composeLines(model, snapshot.applied);
    output.textContent = result.lines.join('\n');
    commandOutput.textContent = result.command;
    const same = !!result.command && result.command === appliedCommand;
    commandBlock.hidden = same || !result.command; sameNote.hidden = !same;
    errors.textContent = result.errors.join('\n'); errors.hidden = !result.errors.length;
    warning.hidden = !model.http.trim();
    proxyHint.hidden = !model.https.trim() || result.lines.some(line => line.startsWith('MULTITAP_TRUSTED_PROXIES='));
    help?.querySelector('[data-access-outside-steps]')?.replaceChildren(...outsideSteps(helpPorts()));
  };
  const field = (label, key, placeholder, hint = null) => addressField(label, 'compose.' + key, model[key],
    value => { model[key] = value; model.note = ''; note.textContent = ''; update(); }, { type: 'url', placeholder, hint });
  const copyOf = (source, select) => async () => {
    try {
      // HTTP pages are no secure context: select the text for a manual copy.
      if (!globalThis.isSecureContext || typeof globalThis.navigator?.clipboard?.writeText !== 'function') throw Error();
      await globalThis.navigator.clipboard.writeText(source.textContent); model.note = t('server-access.124');
    } catch { select(); model.note = t('server-access.125'); }
    note.textContent = model.note;
  };
  const copy = copyOf(output, () => { output.focus?.(); output.select?.(); });
  const copyCommand = copyOf(commandOutput, () => { const range = globalThis.document.createRange?.(); const selection = globalThis.getSelection?.(); if (range && selection) { range.selectNodeContents(commandOutput); selection.removeAllRanges(); selection.addRange(range); } });
  const commandBlock = el('div', { class: 'server-access-field', 'data-access-command-block': '' }, el('span', {}, t('server-access.126')), el('div', { class: 'shell-command' }, commandOutput, button(t('server-access.127'), copyCommand, 'quiet', 'clipboard', { 'data-access-command-copy': '' })));
  const current = snapshot.applied ? ['https', 'http'].map(kind => [kind.toUpperCase(), snapshot.applied[kind].enabled ? listOf(snapshot.applied[kind]).join('\n') : t('server-access.128')])
    : [[t('server-access.129'), t('server-access.130')]];
  const helpPorts = () => { const url = origin(withScheme(String(model.http ?? '').split(',')[0], 'http'), 'http'); return { httpPort: url ? url.port || '80' : null, udpPort: mediaPort(snapshot), compose: true }; };
  const help = outsideHelp(helpPorts());
  const view = panel({ title: t('server-access.131'), className: 'server-access-compose', actions: [el('code', { class: 'mono-chip' }, '.env')] },
    facts([...current, [t('server-access.132'), snapshot.managementOrigin || t('common.17')]]),
    el('p', { class: 'field-hint' }, t('server-access.133')),
    field(t('server-access.134'), 'https', 'https://vdi.example.com'),
    field(t('server-access.135'), 'http', 'http://192.168.0.10:18480', t('server-access.136')),
    warning, errors,
    commandBlock, sameNote,
    note,
    el('details', { class: 'disclosure', 'data-keep-open': 'compose-env' }, el('summary', {}, t('server-access.137')),
      el('div', { class: 'compose-env-body' },
        el('p', { class: 'field-hint' }, t('server-access.138')),
        el('label', { class: 'server-access-field' }, el('span', {}, t('server-access.139')), output),
        proxyHint,
        el('div', { class: 'actions server-access-actions' }, button(t('common.27'), copy, 'secondary', 'clipboard', { 'data-access-env-copy': '' })))),
    el('p', { class: 'field-hint' }, t('server-access.140')));
  update();
  return [view, help];
}

export function render(ctx) {
  const resource = ctx.resource('serverAccess'), snapshot = normalized(ctx.value('serverAccess'));
  const header = ctx.pageHeader({ title: t('common.12'), back: ctx.setupRequired() ? null : ctx.link(['← ', t('common.11')], 'settings', null, { class: 'back-link' }) });
  header.classList.add('server-access-header');
  if (!validResponse(snapshot)) return [header, panel({ title: t('server-access.142') },
    resource?.error ? el('p', { class: 'is-error', role: 'alert' }, resource.error) : loadingState(t('server-access.143')),
    button(t('server-access.144'), () => ctx.load('serverAccess', { force: true }), 'secondary', 'refresh'))];
  ctx.state.ui ??= {};
  if (snapshot.applySupported === false && snapshot.deploymentKind === 'docker' && snapshot.state !== 'recovery_required') return [header, ...composeView(ctx, snapshot)];
  const model = stateFor(ctx, snapshot), disabled = locked(model, snapshot);
  const portMetadata = () => ({ dockerPorts: snapshot.dockerPorts,
    plannedDockerPorts: snapshot.plannedDockerPorts?.length ? snapshot.plannedDockerPorts : model.lastTest?.plannedDockerPorts });
  const upstreamText = () => {
    const value = draftConfig(model.draft).https, { dockerPorts, plannedDockerPorts } = portMetadata();
    const actual = dockerMapping(dockerPorts, 'https', value.listenPort), planned = dockerMapping(plannedDockerPorts, 'https', value.listenPort);
    const mapping = actual ?? planned;
    const destination = mapping ? t('server-access.145') + mapping.hostPort + t('server-access.146') + mapping.containerPort + (actual ? '' : t('server-access.147'))
      : value.listenHost + ':' + value.listenPort;
    return t('server-access.148') + destination + t('server-access.149');
  };
  const root = el('div', { class: 'server-access-page' });
  const repaint = () => ctx.render();
  // Every address of every enabled card, main first; an empty main row stays
  // as its "입력 필요" choice, an empty extra row is not offered.
  const primaryOptions = () => {
    const choices = ['https', 'http'].filter(kind => model.draft[kind].enabled).flatMap(kind => [model.draft[kind].origin, ...model.draft[kind].extraOrigins ?? []]
      .map((text, index) => ({ kind, index, value: canonical(text, kind) })).filter(choice => choice.index === 0 || choice.value));
    return [el('option', { value: '', selected: !choices.some(choice => choice.value && choice.value === model.draft.primaryOrigin) }, t('server-access.150')),
      ...choices.map(choice => el('option', { value: choice.value, selected: !!choice.value && model.draft.primaryOrigin === choice.value }, choice.value || t('server-access.151', { toUpperCase: choice.kind.toUpperCase() })))];
  };
  // The browser's own address must stay in the list, or this console closes.
  const consoleOutside = () => {
    const here = snapshot.managementOrigin, config = draftConfig(model.draft);
    const listed = ['https', 'http'].filter(kind => config[kind].enabled).flatMap(kind => listOf(config[kind]));
    return typeof here === 'string' && (origin(here, 'https') ?? origin(here, 'http')) && listed.length && !listed.includes(here) && (model.dirty || snapshot.applied === null)
      ? t('server-access.152', { here }) : '';
  };
  const helpPorts = () => {
    const url = model.draft.http.enabled ? origin(withScheme(model.draft.http.origin, 'http'), 'http') : null;
    return { httpPort: url ? url.port || '80' : null, udpPort: mediaPort(snapshot) };
  };
  const changedView = () => {
    // Address typing keeps its focus/caret, while the separate origin picker
    // immediately reflects the edited addresses instead of stale options.
    root.querySelector('[name="primaryOrigin"]')?.replaceChildren(...primaryOptions());
    const save = root.querySelector('[data-access-save]'); if (save) save.disabled = true;
    const result = root.querySelector('[data-access-test-status]'); if (result) result.textContent = t('server-access.153');
    const status = root.querySelector('[data-access-draft-status]'); if (status) status.textContent = model.dirty ? t('server-access.59') : '';
    root.querySelector('.server-access-checks')?.remove(); root.querySelector('.server-access-error')?.remove();
    const outside = root.querySelector('[data-access-console-outside]'); if (outside) { outside.textContent = consoleOutside(); outside.hidden = !outside.textContent; }
    root.querySelector('[data-access-outside-steps]')?.replaceChildren(...outsideSteps(helpPorts()));
    const table = root.querySelector('[data-access-forward-rows]'); if (table) table.replaceChildren(...forwardRows());
  };
  const edit = (kind, key, value) => {
    if (disabled || !current(ctx, model)) return;
    const previous = model.draft[kind]?.origin;
    model.draft[kind][key] = value;
    if (key === 'listenHost' || key === 'listenPort') model.draft[kind].listenEdited = true;
    if (key === 'origin' && [previous, canonical(previous, kind)].includes(model.draft.primaryOrigin)) model.draft.primaryOrigin = canonical(value, kind);
    if (kind === 'http' && (key === 'origin' || (key === 'enabled' && value)) && followListener(model.draft))
      for (const field of ['listenHost', 'listenPort']) { const input = root.querySelector('[name="http.' + field + '"]'); if (input) input.value = model.draft.http[field]; }
    invalidate(model);
    changedView();
    const port = root.querySelector('[data-access-external="' + kind + '"]'); if (port) port.textContent = t('server-access.91') + portOf(model.draft[kind].origin, kind);
    const upstream = root.querySelector('[data-access-upstream]');
    if (upstream) upstream.textContent = upstreamText();
  };
  const editExtra = (kind, index, value) => {
    if (disabled || !current(ctx, model)) return;
    const list = model.draft[kind].extraOrigins, previous = canonical(list[index], kind);
    list[index] = value;
    if (previous && model.draft.primaryOrigin === previous) model.draft.primaryOrigin = canonical(value, kind);
    invalidate(model); changedView();
    const port = root.querySelector('[data-access-external="' + kind + '.' + index + '"]'); if (port) port.textContent = t('server-access.91') + portOf(value, kind);
  };
  // Leaving a field shows the address with its scheme.
  const settle = (kind, index, input) => {
    const raw = index === null ? model.draft[kind].origin : model.draft[kind].extraOrigins[index], text = withScheme(raw, kind);
    if (text === raw || disabled || !current(ctx, model)) return;
    if (index === null) edit(kind, 'origin', text); else editExtra(kind, index, text);
    input.value = text;
  };
  const config = draftConfig(model.draft), stale = !!resource?.error;
  const recovering = snapshot.state === 'recovery_required', unobserved = snapshot.currentObserved === false;
  const currentRows = snapshot.applied ? ['https', 'http'].map(kind => [kind.toUpperCase(), snapshot.applied[kind].enabled ? listOf(snapshot.applied[kind]).join('\n') : t('server-access.128')]) : [[t('server-access.142'), unobserved ? t('server-access.154') : t('server-access.155')]];
  const currentInfo = panel({ title: unobserved ? t('server-access.156') : recovering ? t('server-access.157') : t('server-access.158'), className: 'server-access-current' }, facts(currentRows),
    facts([[t('server-access.159'), snapshot.applied?.primaryOrigin || t('common.17')], [t('server-access.132'), snapshot.managementOrigin || t('common.17')]]),
    el('p', { class: 'field-hint' }, t('server-access.160')));
  // A save of this page is on its way (waiting, applying or being checked). The server restarts meanwhile.
  const applyingNow = !recovering && pendingStates.has(snapshot.pending?.state ?? model.operation?.state);
  // While it is applied the tested checks stay; "test, then save" is not said over them.
  const testStatus = model.testing ? t('server-access.161') : testCurrent(ctx, model, snapshot) ? t('server-access.162') : model.lastTest?.ok === false ? t('server-access.163') : applyingNow ? '' : t('server-access.164');
  // The names the preflight really returns (update/server-access-deployment.mjs preflight()), then the earlier ones.
  const checkNames = { configuration: t('server-access.165'), managed_installation: t('server-access.checkInstallationF2'), listeners: t('server-access.166'), certificate_identity_and_key: t('server-access.167'), external_origin_tls_and_identity: t('server-access.172'),
    certificate: t('server-access.167'), tls: t('server-access.168'), proxy: t('server-access.169'), management: t('server-access.170'), primary_origin: t('server-access.171'), external_route: t('server-access.172'), idle: t('server-access.173') };
  // The saved settings no longer list the address this page was opened on: where to open the console next.
  const here = snapshot.managementOrigin, moved = typeof here === 'string' && !!here && Array.isArray(model.operation?.origins) && model.operation.origins.length > 0 && !model.operation.origins.includes(here) &&
    !['failed', 'failed_rolled_back'].includes(model.operation.state) && !!(origin(model.operation.primaryOrigin, 'https') ?? origin(model.operation.primaryOrigin, 'http'));
  const testPanel = panel({ title: t('server-access.174'), className: 'server-access-results' },
    el('p', { role: 'status', 'data-access-test-status': '', hidden: !testStatus }, testStatus),
    model.lastTest ? el('ul', { class: 'server-access-checks' }, model.lastTest.checks.map(check => el('li', {},
      icon(check.status === 'pending' ? 'clock' : check.ok ? 'check' : 'alert'), el('span', {}, (checkNames[check.id] ?? t('server-access.175')) + (check.status === 'pending' ? t('server-access.176') : ''),
        bounded(check.message, 400) && check.message ? el('small', {}, check.message) : check.code && check.status !== 'pending' ? el('small', {}, check.code) : null)))) : null);
  const reloadDisabled = disabled && !recovering;
  const reset = async ({ confirmed = false } = {}) => {
    if (reloadDisabled || !current(ctx, model)) return;
    // Unsaved edits are thrown away only after the operator says so.
    if (!confirmed && model.dirty === true) { ctx.modal(t('server-access.177'), [], () => reset({ confirmed: true }), { description: t('server-access.178'), save: t('server-access.179'), danger: true, reload: false }); return; }
    const edits = model.edits;
    await ctx.load('serverAccess', { force: true });
    if (!current(ctx, model) || model.edits !== edits || ctx.resource('serverAccess')?.error) return;
    clearTimeout(model.expiryTimer); ctx.state.ui.serverAccess = null; ctx.render();
  };
  const primary = el('label', { class: 'server-access-field' }, el('span', {}, t('server-access.159')),
    el('select', { name: 'primaryOrigin', 'data-focus': 'access-primary-origin', disabled,
      onchange: event => { model.draft.primaryOrigin = event.target.value; invalidate(model); repaint(); } },
      primaryOptions()));
  const proxies = el('details', { class: 'server-access-advanced', 'data-keep-open': 'access-proxies' },
    el('summary', {}, t('server-access.180')),
    el('label', { class: 'server-access-field' }, el('span', {}, t('server-access.181')),
      el('textarea', { name: 'trustedProxies', rows: 3, maxlength: 8192, disabled, 'data-focus': 'access-trusted-proxies', spellcheck: 'false',
        oninput: event => { if (!disabled) { model.draft.trustedProxies = event.target.value; invalidate(model); changedView(); } } }, model.draft.trustedProxies)),
    el('p', { class: 'field-hint' }, t('server-access.182')));
  const operation = el('p', { class: 'server-access-operation', role: 'status', 'data-access-draft-status': '' }, statusText(model, snapshot));
  const waiting = !recovering && (snapshot.pending?.state ?? model.operation?.state ?? snapshot.state) === 'waiting_idle';
  // While it waits the page says for what and until when, and offers 취소.
  // Without a reason the server is already ending connections and applying.
  const blockers = waiting ? blockersOf(snapshot) : [], until = blockers.length ? clock(snapshot.waitUntil) : null;
  const pendingOperation = snapshot.pending?.operationId ?? snapshot.operationId ?? model.operation?.operationId;
  const waitingText = blockers.length
    ? [...blockerLines(blockers), until ? t('server-access.183', { until }) : t('server-access.184')].join('\n')
    : t('server-access.185');
  const form = panel({ title: t('common.148'), className: 'server-access-editor', actions: recovering || snapshot.applied === null || model.dirty ? [stateDot(recovering ? t('server-access.186') : snapshot.applied === null ? t('server-access.187') : t('server-access.188'), 'amber')] : [] },
    waiting ? el('div', { class: 'server-access-waiting' },
      el('p', { class: 'server-access-warning', role: 'status', 'data-access-waiting': '' }, waitingText),
      blockers.length ? disable(button(model.cancelling ? t('server-access.190') : t('common.135'), () => cancelPending(ctx, model, pendingOperation), 'secondary', null, { 'data-access-cancel': '' }), model.cancelling ? t('server-access.191') : null) : null) : null,
    endpointCard(ctx, model, snapshot, 'https', edit, repaint, disabled, { editExtra, settle }), endpointCard(ctx, model, snapshot, 'http', edit, repaint, disabled, { editExtra, settle }), primary,
    el('p', { class: 'server-access-warning', role: 'status', 'data-access-console-outside': '', hidden: !consoleOutside() }, consoleOutside()), proxies,
    moved ? el('p', { class: 'server-access-warning', role: 'status', 'data-access-moved': '' }, t('server-access.movedF2'), ' ',
      el('a', { href: model.operation.primaryOrigin + '/admin/', rel: 'noopener' }, model.operation.primaryOrigin)) : null,
    model.conflict ? el('p', { class: 'server-access-warning', role: 'alert' }, t('server-access.192')) : null,
    snapshot.applySupported === false ? el('p', { class: 'server-access-warning' }, recovering ? t('server-access.193') : t('server-access.194')) : null,
    // No answer while a save is applied is the restart it causes, not a failure to report in red.
    stale ? applyingNow ? el('p', { class: 'field-hint', role: 'status', 'data-access-restarting': '' }, t('server-access.restartingF2')) : el('p', { class: 'is-error', role: 'alert' }, t('server-access.195')) : null,
    model.validation.length || model.error ? el('div', { class: 'server-access-error is-error', role: 'alert' }, model.error || model.validation.join('\n')) : null,
    operation,
    el('div', { class: 'actions server-access-actions' },
      disable(button(t('server-access.196'), reset, 'quiet', 'refresh'), reloadDisabled ? t('server-access.197') : null),
      disable(button(model.testing ? t('server-access.161') : t('server-access.198'), () => testDraft(ctx, model, snapshot), 'secondary', 'check', { 'data-access-test': '' }), disabled || stale || model.conflict || model.testing ? t('server-access.96') : null),
      disable(button(t('server-access.42'), () => saveDraft(ctx, model, snapshot), 'primary', null, { 'data-access-save': '' }), disabled || stale || !model.dirty || !testCurrent(ctx, model, snapshot) ? t('server-access.199') : null)));
  const assistEdit = (key, value) => { model.assist[key] = value; const table = root.querySelector('[data-access-forward-rows]'); if (table) table.replaceChildren(...forwardRows()); };
  const forwardRows = () => forwardingRows(draftConfig(model.draft), model.assist, portMetadata()).map(row => el('tr', {}, el('th', { scope: 'row' }, row.kind), el('td', {}, 'TCP ' + row.external), el('td', {}, row.destination), el('td', {}, row.proxy ? t('server-access.200') : row.mapping || t('server-access.201'))));
  const forwarding = el('details', { class: 'server-access-forward panel', 'data-keep-open': 'access-forwarding' },
    el('summary', {}, t('server-access.202')), el('p', { class: 'field-hint' }, t('server-access.203')),
    el('div', { class: 'server-access-bind' }, addressField(t('server-access.204'), 'assist.targetIp', model.assist.targetIp, value => assistEdit('targetIp', value), { max: 64 }),
      model.draft.https.enabled && model.draft.https.mode === 'proxy' ? addressField(t('server-access.205'), 'assist.proxyPort', model.assist.proxyPort, value => assistEdit('proxyPort', value), { type: 'number', max: 5 }) : null),
    el('div', { class: 'server-access-table-wrap' }, el('table', {}, el('thead', {}, el('tr', {}, [t('server-access.206'), t('server-access.207'), t('server-access.208'), t('server-access.listenColumnF2')].map(text => el('th', { scope: 'col' }, text)))), el('tbody', { 'data-access-forward-rows': '' }, forwardRows()))),
    config.https.enabled && config.https.mode === 'proxy' ? el('p', { class: 'field-hint', 'data-access-upstream': '' }, upstreamText()) : null);
  root.append(el('div', { class: 'server-access-layout' }, form, el('aside', { class: 'server-access-side' }, currentInfo, testPanel)), outsideHelp(helpPorts()), forwarding);
  return [header, root];
}
