// Maintenance and settings: 업데이트 (the server and every Host), and the pages
// of the sidebar's 설정 group that live here: Proxmox 연결, 서버 정보 and
// 오픈소스 라이선스. Each is entered from the sidebar; there
// is no 설정 overview page (nothing may hide one level down).
import { el, icon, button, stateDot, panel, facts, date, disable, selectVisibleText } from './dom.js';
import { messages, updateActive, updateLabels, deploymentLabels, hostUpdateCodeText } from './labels.js';
import { hostUpdateEligible, hostUpdateView, hostUpdateButton, hostUpdateAll, hostUpdateAllLabel, hostUpdateFailed, hostBatchPanel, registeredHostIds } from './hosts.js';
import { healthLine } from './overview.js';
import { createShellSetup, enrollmentCommand, renewalCommand, enableConsoleCommand, dedicatedIdentity, decodeRenewalBlock, commandBox } from '../proxmox-shell-setup.js';
import { t, localNotes } from './i18n.js';


// a.b.c against a.b.c, as numbers.
export function newerVersion(a, b) {
  const left = String(a).split('.').map(Number), right = String(b).split('.').map(Number);
  if (left.length !== 3 || right.length !== 3 || [...left, ...right].some(part => !Number.isInteger(part) || part < 0)) return false;
  for (let index = 0; index < 3; index++) if (left[index] !== right[index]) return left[index] > right[index];
  return false;
}
// The server's part of 업데이트: what runs now, the new version with its notes,
// the check and the apply action (server-update-v1).
function serverUpdate(ctx) {
  const state = ctx.state, info = state.serverInfo, kind = info?.deployment?.kind ?? 'unknown';
  // Docker: the install command's multitap tool first; editing Compose by hand is the fallback.
  const guide = kind === 'docker' ? [
    t('system.1'),
    t('system.dockerCli'),
    t('system.dockerCompose'),
  ] : kind === 'systemd' ? [
    t('system.4'),
    t('system.5'),
    t('system.6'),
    t('system.7'),
  ] : [t('system.8'), t('system.9')];
  const update = info?.update, connected = update?.feedConfigured === true, busy = state.serverUpdateSubmitting || updateActive.has(update?.state) || state.serverUpdatePending;
  const check = disable(button(busy ? t('system.10') : t('system.11'), () => connected ? ctx.checkServerUpdate() : ctx.loadServerInfo(), 'secondary', 'refresh'), busy ? t('system.12') : state.serverInfoLoading ? t('system.13') : null);
  const apply = disable(button(t('common.8'), ctx.applyServerUpdate, 'primary', 'download'), busy ? t('system.12') : update?.state !== 'available' ? t('system.14') : !update.applySupported ? t('system.15') : null);
  const stepsList = el('ol', { class: 'steps-list' }, guide.map(step => el('li', {}, step)));
  const copy = button(t('system.16'), async () => { try { if (!globalThis.navigator?.clipboard?.writeText) throw Error('unavailable'); await globalThis.navigator.clipboard.writeText(guide.map((step, i) => `${i + 1}. ${step}`).join('\n')); ctx.notice(t('system.17')); } catch { selectVisibleText(stepsList); ctx.notice(t('system.18'), true); } }, 'quiet');
  const stateTone = update?.state === 'succeeded' || update?.state === 'current' ? 'green' : ['failed', 'rolled_back', 'recovery_required'].includes(update?.state) ? 'red' : 'amber';
  // A Docker Compose install only checks the signed release feed; the operator runs the commands.
  const manual = update?.channel === 'docker-release-feed';
  // A metadata check has no selected release or apply operation. Keep a real
  // installation failure visible even when it failed before operationId existed.
  const checkFailed = update?.state === 'failed' && !update.operationId && !update.latest && !update.releaseId && update.errorCode !== 'update_interrupted';
  // A check that failed is 확인 실패 on every install: nothing was updated (업데이트 실패 is an update that did not finish).
  const stateLabel = (manual || checkFailed) && update.state === 'failed' ? t('system.19') : updateLabels[update?.state] ?? t('common.17');
  const current = panel({ title: t('system.20'), className: 'server-current' },
    facts([[t('system.21'), info?.version ? el('span', { class: 'mono-value' }, info.version) : t('common.17')], [t('system.22'), el('span', { title: kind === 'unknown' ? t('system.23') : null }, deploymentLabels[kind])], ...(info?.transport?.protocol === 'broker-transport-v1' ? [[t('common.148'), info.transport.scheme === 'http' ? t('system.25') : info.transport.directTls ? t('system.26') : t('system.27')], [t('common.103'), info.transport.publicOrigin], [t('system.28'), el('span', { title: kind === 'docker' ? t('system.29') : t('system.30') }, Number.isInteger(info.listener?.port) ? String(info.listener.port) : t('common.17'))]] : []), [t('system.31'), info ? date(info.checkedAt) : '—']]),
    // The line under the table: only while there is nothing to show yet, or for a failed read. A repeated read
    // (every 1.5 s during an update) must not add and remove it, and the restart of an update is told once, as
    // the progress on the right, not here as an error as well.
    state.serverInfoLoading && !info ? el('p', { role: 'status', class: 'muted panel-foot' }, t('system.32')) : state.serverInfoError && !busy ? el('p', { class: 'is-error panel-foot', role: 'alert' }, state.serverInfoError) : null);
  // This server runs a release newer than the newest one the feed lists (a pre-release, or a build of one's own).
  const ahead = !!info?.version && !!update?.latest?.version && update.state !== 'available' && newerVersion(info.version, update.latest.version);
  const release = panel({ title: update?.state === 'available' && update.latest ? t('system.33') + update.latest.version : ahead ? t('system.latestPublic') + update.latest.version : update?.latest ? t('common.34') + update.latest.version : t('common.8'), description: update?.latest?.releasedAt ? t('system.34', { date: date(update.latest.releasedAt) }) : null, className: 'server-release', actions: ['current', 'available'].includes(update?.state) || !connected ? [] : [stateDot(stateLabel, stateTone)] },
    connected ? el('div', {},
      ahead ? el('p', { class: 'release-current', 'data-server-ahead': '' }, t('system.newerThanLatest', { version: info.version, latest: update.latest.version })) : update?.state === 'current' ? el('p', { class: 'release-current' }, t('system.35')) : null,
      update?.latest ? el('div', {}, el('h3', {}, t('common.104')), el('p', { class: 'release-notes' }, localNotes(update.latest.changelog))) : update?.state === 'current' || busy ? null : el('p', {}, t('system.36')),
      busy ? el('div', { role: 'status', class: 'progress-block' }, el('p', {}, state.serverInfoError || updateLabels[update?.state] || t('system.37')), el('progress', { ...(update?.state === 'downloading' && update.totalBytes > 0 ? { max: update.totalBytes, value: update.downloadedBytes ?? 0 } : {}) })) : null,
      update?.errorCode ? el('p', { class: 'is-error', role: 'alert' }, manual ? (update.errorCode === 'update_signature_invalid' ? t('system.38') : t('system.39'))
        : update.errorCode === 'server_update_unavailable' ? messages.server_update_unavailable
          : update.state === 'rolled_back' ? t('system.40') : update.state === 'recovery_required' ? t('system.41') : checkFailed ? t('system.39') : t('system.42')) : null,
      ...(manual ? [dockerSteps(update)] : update?.state === 'available' || busy ? [el('div', { class: 'actions' }, apply), el('p', { class: 'field-hint' }, t('system.43'))] : []))
      : el('div', {}, el('p', {}, t('system.44')), el('p', { class: 'muted' }, t('system.45'))));
  return { check, manual, body: [el('div', { class: 'detail-grid' }, current, release),
    ...(!connected ? [panel({ title: deploymentLabels[kind] + t('system.46'), actions: [copy] }, stepsList)] : [])] };
}

// Desktops whose Host can be updated from here (hosts.js), with the bulk
// update and the ones that failed last time.
function hostUpdates(ctx) {
  const data = ctx.state.data;
  const row = (iconName, tone, title, text, actions = []) => el('div', { class: 'row-line' }, el('span', { class: 'tile' + (tone ? ' ' + tone : '') }, icon(iconName)),
    el('div', { class: 'row-text' }, el('strong', {}, title), text ? el('small', {}, text) : null), actions.filter(Boolean).length ? el('div', { class: 'row-actions' }, actions) : null);
  const open = (label, cls, filter) => el('a', { href: ctx.hashFor('desktops'), class: 'button ' + cls, onclick: event => { if (event.ctrlKey || event.metaKey || event.shiftKey) return; event.preventDefault?.(); ctx.state.ui.desktops = { ...(ctx.state.ui.desktops ?? { byNode: false }), query: '', filter }; ctx.navigate('desktops'); } }, label);
  const registered = registeredHostIds(ctx), latest = data.hostUpdates?.latest, eligible = hostUpdateEligible(ctx);
  const views = (registered ? data.desktops.filter(d => registered.has(d.id)) : []).map(d => ({ d, view: hostUpdateView(ctx, d) }));
  const inUse = eligible.filter(d => hostUpdateView(ctx, d).sessionActive).length;
  const hostRow = eligible.length && latest ? row('download', 'warn', t('system.47', { eligibleCount: eligible.length, version: latest.version }), t('system.49', { value: inUse ? t('system.48', { inUse }) : '' }), [open(t('system.50'), 'secondary small', 'update'), button(hostUpdateAllLabel(eligible.length), () => hostUpdateAll(ctx), 'primary small')])
    // No registered desktop at all is not "every desktop has the latest Host".
    : latest && registered && !views.length ? row('download', '', t('system.52', { version: latest.version }), t('system.noDesktops'))
    : latest ? row('download', registered ? 'good' : '', registered ? t('system.51', { version: latest.version }) : t('system.52', { version: latest.version }), registered || ctx.serverDown?.() === true ? null : t('system.53'))
    : row('download', '', t('system.54'), t('system.55'));
  const failedRows = views.filter(({ view }) => view?.available && hostUpdateFailed(view))
    .map(({ d, view }) => row('alert', 'bad', t('system.56', { name: d.name }), hostUpdateCodeText[view.operation.code] ?? t('system.57'), [hostUpdateButton(ctx, d, 'secondary small')]));
  if (views.some(({ view }) => ['waiting', 'dispatched', 'accepted', 'downloading', 'verifying', 'installing'].includes(view?.operation?.status))) ctx.scheduleHostUpdatePoll();
  const batch = ctx.state.ui.hostBatch ? hostBatchPanel(ctx, ctx.state.ui.hostBatch, () => { ctx.state.ui.hostBatch = null; ctx.render(); }) : null;
  return [panel({ title: t('common.33'), className: 'settings-updates' }, el('div', { class: 'row-list' }, hostRow, failedRows)), batch];
}
// How many things 업데이트 can apply now: a newer server (0 or 1) and the
// desktops whose Host can be updated. The sidebar badge shows the sum.
export function updateCounts(ctx, serverVersion = null) {
  const hosts = ctx.state.data ? hostUpdateEligible(ctx).length : 0;
  return { server: serverVersion ? 1 : 0, hosts, total: (serverVersion ? 1 : 0) + hosts };
}
// 업데이트: the server first, then the Hosts.
export function updates(ctx) {
  // The server does not answer (app.js, the state of its banner "서버가 응답하지 않아요"): one steady line until it
  // answers again, instead of a read that fails every few seconds and a guide for an install of an unknown kind.
  if (ctx.serverDown?.() === true) return [ctx.pageHeader({ title: t('app.updatesPage'), actions: [disable(button(t('system.11'), () => {}, 'secondary', 'refresh'), t('system.serverDownHint'))] }),
    panel({ title: t('system.20'), className: 'server-current server-down' }, el('p', { role: 'status', class: 'panel-foot', 'data-server-down': '' }, el('span', { class: 'loading-dot is-inline', 'aria-hidden': 'true' }), t('system.serverDown'))),
    ...hostUpdates(ctx)];
  const server = serverUpdate(ctx);
  return [ctx.pageHeader({ title: t('app.updatesPage'), description: server.manual ? t('system.59') : null, actions: [server.check] }),
    ...server.body, ...hostUpdates(ctx)];
}

// Docker Compose: the exact commands for the verified newer release, run by the operator.
// A Docker server made with the install command updates with its own tool (multitap update: a backup, then
// the new release); the Compose commands are for a server whose Compose files are kept by hand.
function dockerSteps(update) {
  if (update.state !== 'available' || !Array.isArray(update.manual?.commands)) return null;
  return el('div', { class: 'docker-update' },
    el('h3', {}, t('system.61')),
    el('p', {}, t('system.dockerCliStep')),
    commandBox(el, button, 'sudo multitap update'),
    el('p', { class: 'field-hint' }, t('system.63')),
    el('details', {}, el('summary', {}, t('system.dockerComposeSummary')), el('p', {}, t('system.62')), ...update.manual.commands.map(command => commandBox(el, button, command))),
    Array.isArray(update.manual.sourceCommands) ? el('details', {}, el('summary', {}, t('system.64')), ...update.manual.sourceCommands.map(command => commandBox(el, button, command))) : null);
}

const unlimitedExpiry = config => config.expirySource === 'proxmox' && config.accountExpiryKnown === true && config.tokenExpiresAt === null && config.accountExpiresAt === null;
// The raw form: a fingerprint is taken as Proxmox shows it, in either case.
function formInput(node) {
  const input = Object.fromEntries(new FormData(node));
  if (typeof input.fingerprint === 'string') input.fingerprint = input.fingerprint.trim().toUpperCase();
  return input;
}
async function saveProxmox(ctx, input) {
  if (ctx.state.pveBusy) throw new Error(t('system.65'));
  const action = ctx.pageAction('proxmox-save');
  let saved = false;
  try {
    await ctx.api('/proxmox', 'PUT', input); action.check(); saved = true;
    ctx.state.pveLoaded = false; ctx.state.pveError = '';
    const inventory = await ctx.api('/inventory'); action.check(); ctx.state.data = inventory;
    const result = await ctx.syncProxmox({ notify: false }); action.check();
    if (ctx.state.pveError) throw new Error(ctx.state.pveError);
    const count = result?.unavailableCount ?? ctx.state.data?.proxmox?.unavailableCount;
    if (Number.isSafeInteger(count) && count > 0) ctx.notice(t('system.66', { count }), true);
    else ctx.notice(count === 0 ? t('system.67') : t('system.68'));
  } catch (error) {
    action.check();
    if (saved) throw new Error(t('system.69') + error.message);
    throw error;
  }
}
// Saving over a working connection asks once. The promise settles with the
// save, or rejects when the dialog is closed without saving.
const overwriteText = t('system.70');
function confirmOverwrite(ctx, run) {
  return new Promise((resolve, reject) => {
    let settled = false;
    ctx.confirmAction(t('system.71'), overwriteText, async () => { await run(); settled = true; resolve(); }, t('system.72'), true);
    globalThis.document.querySelector('#modal')?.addEventListener?.('close', () => { if (!settled) { settled = true; reject(new Error(t('system.73'))); } }, { once: true });
  });
}
function renewProxmox(ctx) {
  if (ctx.state.pveBusy) { ctx.notice(t('system.74'), true); return; }
  const config = ctx.state.data.proxmox;
  const action = ctx.pageAction('proxmox-renew');
  const command = renewalCommand(globalThis.location.origin, config.tokenId, config.endpoint, { allowInsecureHttp: globalThis.location.protocol === 'http:', sha256: config.setupScriptSha256 });
  ctx.modal(t('system.75'), [
    { label: t('system.76'), value: command, command: true },
    { name: 'renewal', label: t('system.77'), multiline: true, required: false, max: 32768, placeholder: t('system.78') },
  ], async input => {
    action.check();
    if (ctx.state.pveBusy) throw new Error(t('system.65'));
    let saved = false;
    try {
      if (input.renewal.trim()) {
        const result = decodeRenewalBlock(input.renewal, config.tokenId);
        await ctx.api('/proxmox', 'PUT', { endpoint: config.endpoint, tokenId: config.tokenId, ca: config.ca || '', expiresAt: result.expiresAt });
        action.check(); saved = true;
      }
      await ctx.syncProxmox(); action.check();
      if (ctx.state.pveError) throw new Error(ctx.state.pveError);
      const result = ctx.state.data.proxmox;
      if (result.state === 'error') throw new Error(messages[result.errorCode] || t('common.37'));
      if (!result.expiresAt && !unlimitedExpiry(result)) throw new Error(t('system.79'));
      // Nothing pasted and the same expiry as when this dialog opened: the command was not run, so nothing is confirmed.
      if (!input.renewal.trim() && (result.expiresAt ?? null) === (config.expiresAt ?? null)) throw new Error(t('system.renewUnchangedF2'));
      ctx.notice(result.expirySource === 'proxmox' ? t('system.80') : t('system.81'));
    } catch (error) {
      action.check();
      if (saved) throw new Error(t('system.82') + error.message);
      throw error;
    }
  }, { save: t('system.83'), description: t('system.84') });
}
// The integration account lacks VM.Console (an installation connected before
// the setup script granted it, or an account made by hand): the page says so
// where the account is shown, with the one command that adds it, and this
// asks Proxmox again once the command has run.
async function checkConsolePermission(ctx) {
  const ui = ctx.state.ui;
  if (ui.consoleChecking) return;
  const action = ctx.pageAction('console-permission');
  ui.consoleChecking = true; ui.consoleCheck = ''; ctx.render();
  try {
    const result = await ctx.api('/proxmox/console-permission', 'POST', {});
    action.check(); await ctx.refresh(); action.check();
    if (result.granted) ctx.notice(t('system.85'));
    // What is still missing stays next to the command, not in a passing message.
    else ui.consoleCheck = t('system.86');
  } catch (error) { if (action.current()) ctx.notice(error.message, true); }
  finally { if (action.current()) { ui.consoleChecking = false; if (ctx.state.user) ctx.render(); } }
}

export function proxmox(ctx) {
  const state = ctx.state, config = state.data.proxmox, setup = ctx.setupRequired();
  const sync = disable(button(state.pveBusy ? t('common.86') : t('common.42'), ctx.syncProxmox, 'secondary', 'refresh'), state.pveBusy ? t('common.149') : !config.configured ? t('system.87') : null);
  const renew = disable(button(t('system.75'), () => renewProxmox(ctx), 'primary', 'key'), state.pveBusy ? t('system.88') : null);
  // `working`: a saved connection that has been checked at least once (the first setup ends with that check). Settings
  // saved during the first setup that never connected are still the first connection: the two steps stay open,
  // pasting again replaces them without the "바꾸기" confirmation, and there is no token to renew yet.
  const working = config.configured && !setup;
  const result = [ctx.pageHeader({ title: t('common.10'), description: setup ? t('system.89') : null, actions: working ? [sync, renew] : config.configured ? [sync] : [] })];
  // The first setup in the order of its bar and of 홈 시작하기: the account, the server's address, Proxmox (this page).
  // The address step is ticked once the server has one (a Docker install made by the install command starts with it).
  if (setup) {
    const addressed = !!ctx.value?.('serverAccess')?.applied;
    result.push(el('ol', { class: 'onboarding-steps', 'aria-label': t('system.91') }, el('li', { class: 'complete' }, icon('check'), t('system.92')),
      el('li', { class: addressed ? 'complete' : '' }, addressed ? icon('check') : null, ctx.link(t('system.setupStepAccessF2'), 'server-access')),
      el('li', { class: 'current', 'aria-current': 'step' }, t('system.setupStepProxmoxF2'))));
  }
  if (config.configured) {
    const expiry = config.expiresAt && Number.isFinite(Date.parse(config.expiresAt)) ? new Date(config.expiresAt) : null;
    const unlimited = unlimitedExpiry(config);
    const days = expiry ? Math.ceil((expiry.getTime() - Date.now()) / 86400000) : null;
    const status = state.pveBusy ? stateDot(t('common.30'), 'blue') : state.pveError || config.state === 'error' ? stateDot(t('system.95'), 'red') : config.state === 'ok' ? stateDot(t('system.96'), 'green') : stateDot(t('common.17'), 'amber');
    const item = (label, value, detail, tone = '') => el('div', { class: 'pve-detail' + (tone ? ' is-' + tone : '') }, el('span', {}, label), el('strong', {}, value), detail ? el('small', {}, detail) : null);
    // pve-health-v1: a Proxmox call refused on the certificate (power, Console,
    // status, not only this page's check) counts as this page's failure too.
    const refused = ['changed', 'expired', 'untrusted'].includes(config.certificateFailure?.kind) ? config.certificateFailure.kind : null;
    const failure = state.pveError || (config.state === 'error' ? messages[config.errorCode] ?? t('common.37') : '') || (refused ? t('pveHealth.' + refused + 'Fix') : '');
    // A pinned connection (the setup script remembered the node's exact
    // certificate) fails on the certificate once Proxmox replaces it: say that,
    // and give the one command that makes a new connection setting.
    const failureCode = state.pveError ? state.pveErrorCode : config.state === 'error' ? config.errorCode : refused ? 'proxmox_certificate_error' : null;
    let pinCommand = null;
    if (config.fingerprint && failureCode === 'proxmox_certificate_error') try { pinCommand = enrollmentCommand(globalThis.location.origin, { allowInsecureHttp: globalThis.location.protocol === 'http:', sha256: config.setupScriptSha256 }); } catch { /* the plain line below */ }
    // Past its date: renew it in Proxmox first, then the same command.
    const pinExpired = refused === 'expired';
    const failureAlert = !failure || state.pveBusy ? null : config.fingerprint && failureCode === 'proxmox_certificate_error'
      ? el('div', { class: 'inline-alert is-error pin-changed-alert', role: 'alert' }, icon('alert'), el('div', {}, el('strong', {}, pinExpired ? t('pveHealth.expired') : t('system.97')),
          el('span', {}, pinExpired ? t('pveHealth.expiredPinned') : t('system.98')),
          pinCommand ? commandBox(el, button, pinCommand) : null), button(t('common.32'), ctx.syncProxmox, 'quiet', 'refresh'))
      : el('div', { class: 'inline-alert is-error', role: 'alert' }, icon('alert'), el('div', {}, el('strong', {}, refused && failureCode === 'proxmox_certificate_error' && !state.pveError && config.state !== 'error' ? t('pveHealth.' + refused) : t('system.99')), el('span', {}, failure)), button(t('common.32'), ctx.syncProxmox, 'quiet', 'refresh'));
    // The pinned certificate's own expiry (pve-health-v1), next to the token's.
    const pinExpiry = config.pinnedCertificate?.expiresAt && Number.isFinite(Date.parse(config.pinnedCertificate.expiresAt)) ? new Date(config.pinnedCertificate.expiresAt) : null;
    const pinDays = pinExpiry ? Math.ceil((pinExpiry.getTime() - Date.now()) / 86400000) : null;
    // VM.Console: granted by the setup script from the start. Missing only on an
    // older connection or an account made by hand; then one line and one command.
    const consoleMissing = config.consolePrivilege === 'missing', consoleChecking = state.ui?.consoleChecking === true;
    let consoleCommand = null;
    if (consoleMissing && dedicatedIdentity(config.tokenId)) try { consoleCommand = enableConsoleCommand(globalThis.location.origin, config.tokenId, { allowInsecureHttp: globalThis.location.protocol === 'http:', sha256: config.setupScriptSha256 }); } catch { /* no command for this address: the manual line below */ }
    result.push(el('section', { class: 'panel pve-connection-card' },
      el('div', { class: 'panel-head' }, el('div', { class: 'pve-identity' }, el('span', { class: 'connection-icon' }, icon('cube')), el('div', {}, el('h2', {}, 'Proxmox VE'), el('p', {}, config.endpoint))), status),
      failureAlert,
      config.agentPrivilegeMissing && !state.pveBusy ? el('div', { class: 'inline-alert is-warn', role: 'status' }, icon('alert'), el('div', {}, el('strong', {}, t('system.100')), el('span', {}, t('system.101')), commandBox(el, button, renewalCommand(globalThis.location.origin, config.tokenId, config.endpoint, { allowInsecureHttp: globalThis.location.protocol === 'http:', sha256: config.setupScriptSha256 })))) : null,
      consoleMissing && !state.pveBusy ? el('div', { class: 'inline-alert is-warn console-permission-alert', role: 'status' }, icon('alert'), el('div', {}, el('strong', {}, t('system.102')),
        el('span', {}, consoleCommand ? t('system.103') : t('system.104')),
        consoleCommand ? commandBox(el, button, consoleCommand) : null,
        state.ui?.consoleCheck ? el('small', { class: 'is-error', role: 'alert' }, state.ui.consoleCheck) : null),
        disable(button(consoleChecking ? t('common.86') : t('system.105'), () => checkConsolePermission(ctx), 'secondary', 'refresh'), consoleChecking ? t('system.106') : null)) : null,
      el('div', { class: 'pve-details' }, item(t('system.107'), config.userId || String(config.tokenId ?? '').split('!')[0] || t('common.17'), config.tokenId), item(t('system.108'), config.checkedAt ? date(config.checkedAt) : t('common.17'), config.vmCount !== null && config.vmCount !== undefined ? t('system.109', { vmCount: config.vmCount }) : null),
        item(t('system.110'), unlimited ? t('system.111') : days === null ? t('common.17') : days <= 0 ? t('common.113') : t('system.112', { days }), expiry ? t('system.113', { date: date(expiry) }) : unlimited ? t('system.114') : t('system.115'), days !== null && days <= 7 ? 'warn' : ''),
        pinExpiry ? item(t('pveHealth.detail'), pinDays <= 0 ? t('common.113') : t('system.112', { days: pinDays }), t('system.113', { date: date(pinExpiry) }), pinDays <= 30 ? 'warn' : '') : null,
        item(t('system.116'), config.consolePrivilege === 'granted' ? t('common.117') : consoleMissing ? t('common.36') : t('common.17'), consoleMissing ? t('system.117') : config.consolePrivilege === 'granted' ? t('system.118') : t('system.119'), consoleMissing ? 'warn' : '')),
      el('div', { class: 'pve-card-footer' }, el('small', {}, config.expirySource === 'proxmox' ? t('system.120') : expiry ? t('system.121') : t('system.122')), setup ? null : ctx.link(t('system.123'), 'desktops', null, { class: 'button quiet' }))));
  }
  const field = (label, name, value, placeholder, type = 'text') => el('label', {}, label, el('input', { name, type, value: value || '', placeholder, required: type !== 'password' || !config.configured, autocomplete: type === 'password' ? 'new-password' : 'off', maxlength: name === 'secret' ? 256 : 200 }));
  const form = el('form', { class: 'pve-raw-form', onsubmit: async event => {
    event.preventDefault(); const action = ctx.pageAction('proxmox-form'), submit = event.target.querySelector('button[type=submit]');
    if (working) { const node = event.target, input = formInput(node); ctx.confirmAction(t('system.71'), overwriteText, async () => { await saveProxmox(ctx, input); node.reset(); }, t('system.72'), true); return; }
    submit.disabled = true;
    try { await saveProxmox(ctx, formInput(event.target)); action.check(); event.target.reset(); }
    catch (error) { if (action.current() && error.code !== 'admin_session_changed') ctx.notice(error.message, true); }
    finally { if (action.current()) submit.disabled = false; }
  } }, el('div', { class: 'form-grid' }, field(t('system.124'), 'endpoint', config.endpoint, 'https://pve.example.com:8006'), field(t('system.125'), 'tokenId', config.tokenId, 'multitap@pve!broker')), field(t('system.126'), 'secret', '', config.configured ? t('system.127') : t('system.128'), 'password'), el('label', {}, t('system.129'), el('textarea', { name: 'ca', placeholder: '-----BEGIN CERTIFICATE-----', maxlength: 16000 }, config.ca || '')), el('label', {}, t('system.130'), el('input', { name: 'fingerprint', type: 'text', value: config.fingerprint || '', placeholder: t('system.131'), autocomplete: 'off', spellcheck: 'false', maxlength: 95 }), el('small', {}, t('system.132'))), el('button', { type: 'submit', class: 'primary' }, t('system.133')));
  const shell = createShellSetup(el, button, input => working ? confirmOverwrite(ctx, () => saveProxmox(ctx, input)) : saveProxmox(ctx, input), { sha256: config.setupScriptSha256 });
  const raw = el('details', { class: 'pve-disclosure' }, el('summary', {}, t('system.134')), form);
  if (working) result.push(el('details', { class: 'pve-disclosure pve-change', inert: state.pveBusy }, el('summary', {}, t('system.135')), shell, raw));
  else result.push(shell, raw);
  return result;
}

const protections = [
  [t('system.136'), t('system.protectSignIn'), t('system.137')],
  [t('system.138'), t('system.protectForgery'), t('system.139')],
  [t('system.140'), t('system.protectContent'), t('system.141')],
  [t('system.142'), t('system.protectSecrets'), t('system.143')],
  [t('system.144'), t('system.protectDiagnostics'), t('system.145')],
];
// Display only the broker's complete, canonical transport profile. The URL used
// to open this console is not evidence of the configured client address.
function clientTransport(info) {
  const transport = info?.transport;
  if (!transport || typeof transport !== 'object' || Array.isArray(transport) ||
      Object.keys(transport).sort().join(',') !== 'allowInsecureHttp,directTls,protocol,publicOrigin,scheme' ||
      transport.protocol !== 'broker-transport-v1' || !['http', 'https'].includes(transport.scheme) ||
      typeof transport.directTls !== 'boolean' || typeof transport.allowInsecureHttp !== 'boolean' ||
      typeof transport.publicOrigin !== 'string' || transport.publicOrigin.length > 512 ||
      /[\s\x00-\x20\x7f\\@?#%]/u.test(transport.publicOrigin) ||
      (transport.scheme === 'http' ? !transport.allowInsecureHttp || transport.directTls : transport.allowInsecureHttp)) return null;
  try {
    const url = new URL(transport.publicOrigin);
    if (url.protocol !== transport.scheme + ':' || url.origin !== transport.publicOrigin || !url.hostname || url.port === '0') return null;
    return { ...transport, port: url.port || (transport.scheme === 'https' ? '443' : '80') };
  } catch { return null; }
}


// 서버 정보: the address, version and deployment of this server, and the
// protections it always applies.
export function serverInformation(ctx) {
  const info = ctx.state.serverInfo, data = ctx.state.data;
  const transport = clientTransport(info), consoleOrigin = globalThis.location.origin;
  // 서버 다시 시작: only where the server says ending it starts it again (a Docker Compose install without an updater).
  const restarting = !!ctx.state.serverRestart;
  const restart = info?.restart?.supported === true || restarting ? disable(button(t('system.restart'), ctx.restartServer, 'secondary', 'refresh', { 'data-server-restart': '' }), restarting ? t('system.restarting') : null) : null;
  return [ctx.pageHeader({ title: t('common.14') }),
    panel({ title: t('system.147'), className: 'server-facts', actions: [restart, ctx.link(t('system.148'), 'updates', null, { class: 'button quiet' })].filter(Boolean) },
      restarting ? el('p', { role: 'status', class: 'field-hint server-restarting', 'data-server-restarting': '' }, el('span', { class: 'loading-dot is-inline', 'aria-hidden': 'true' }), t('system.restarting')) : null, facts([
      [t('system.149'), transport ? el('span', { class: 'mono-value' }, transport.publicOrigin) : ctx.state.serverInfoLoading ? t('common.30') : t('common.17'), transport ? t('system.transport', { scheme: transport.scheme === 'http' ? t('system.25') : t('system.150'), port: transport.port }) : t('system.153')],
      consoleOrigin !== transport?.publicOrigin ? [t('system.154'), el('span', { class: 'mono-value', title: t('system.155') }, consoleOrigin)] : null,
      [t('system.156'), info?.version ? el('span', { class: 'mono-value' }, info.version) : ctx.state.serverInfoLoading ? t('common.30') : t('common.17')],
      [t('system.22'), deploymentLabels[info?.deployment?.kind ?? 'unknown']],
      [t('system.157'), info ? (info.update.feedConfigured ? t('system.96') : t('system.158')) : t('common.17')],
      ['Proxmox', data.proxmox.configured ? data.proxmox.endpoint : t('system.158'), data.proxmox.configured ? ctx.link(t('common.10'), 'proxmox') : null],
    ])),
    panel({ title: t('system.159'), className: 'server-protections' },
      facts(protections.map(([title, value, text]) => [title, el('span', { title: text }, value)])),
      el('details', { class: 'protection-more', 'data-keep-open': 'server-protections' }, el('summary', {}, t('common.80')),
        el('ul', { class: 'protection-list' }, protections.map(([title, , text]) => el('li', {}, el('strong', {}, title), ' ', text))))),
    healthLine(ctx)];
}

// What this server and its admin console ship, each with its original text.
// The ws version must match server/package.json (a test reads both).
export const serverOpenSource = [
  [t('system.161'), 'AGPL-3.0', '/admin/licenses/Multitap-AGPL-3.0.txt', null],
  ['Node.js', t('system.162'), '/admin/licenses/nodejs-LICENSE.txt', t('system.163')],
  ['ws', 'MIT', '/admin/licenses/ws-LICENSE.txt', '8.22.0 · WebSocket'],
  ['noVNC', t('system.164'), '/admin/licenses/noVNC-LICENSE.txt', t('system.165')],
  ['Pretendard', 'SIL OFL 1.1', '/admin/fonts/LICENSE.txt', t('system.166')],
  [t('system.167'), 'GFDL 1.3', '/admin/help/LICENSE-proxmox.txt', t('system.168')],
];

// 오픈소스 라이선스: every component with a link to its original text.
export function licenses(ctx) {
  const info = ctx.state.serverInfo;
  const row = ([name, license, href, detail]) => [name,
    el('a', { href, target: '_blank', rel: 'noopener' }, license),
    detail ?? (info?.version ? t('common.34') + info.version : null)];
  return [ctx.pageHeader({ title: t('common.15') }),
    panel({ title: t('system.170'), count: serverOpenSource.length, className: 'server-licenses' }, facts(serverOpenSource.map(row)),
      el('p', { class: 'field-hint' }, t('system.171')))];
}
