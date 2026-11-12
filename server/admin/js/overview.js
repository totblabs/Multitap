// 홈: what has to be done now, four numbers, what is used (자원), who is connected, and one quiet
// line about the server. Only values the broker actually reports: an unknown
// value stays marked as unknown or loading, never estimated. Until the first
// user can connect, the steps to get there come first.
import { el, icon, panel, loadingState, emptyState, relative, sinceText, button, board } from './dom.js';
import { messages } from './labels.js';
import { hostUpdateEligible, hostUpdateView, registeredHostIds, requestApproveButton, requestVm, rejectHostRequest, desktopStatus, enrollmentGuide, localPc } from './hosts.js';
import { collect } from './timeline.js';
import { powerForm } from './desktops.js';
import { openLog } from './monitor.js';
import { userForm } from './users.js';
import { resourcePanel } from './resources.js';
import { t } from './i18n.js';

const daysLeft = iso => { const time = Date.parse(iso); return Number.isFinite(time) ? Math.ceil((time - Date.now()) / 86400000) : null; };
// pve-health-v1 (server/src/proxmox-health.mjs): the Proxmox certificate, as
// one item for the banner (app.js) and 홈. A refused certificate comes first
// (red); otherwise a pinned certificate from 30 days before its expiry, red
// from 7. Null when there is nothing to say.
const certificateKinds = ['changed', 'expired', 'untrusted'];
export function proxmoxCertificateNotice(proxmox) {
  if (!proxmox?.configured) return null;
  const kind = proxmox.certificateFailure?.kind;
  if (certificateKinds.includes(kind)) return { tone: 'bad', failure: true, title: t('pveHealth.' + kind), text: t('pveHealth.' + kind + 'Fix') };
  const days = daysLeft(proxmox.pinnedCertificate?.expiresAt);
  if (days === null || days > 30) return null;
  return { tone: days <= 7 ? 'bad' : 'warn', failure: false, days, title: days <= 0 ? t('pveHealth.expired') : t('pveHealth.pinExpiring', { pinDays: days }), text: days <= 0 ? t('pveHealth.expiredFix') : t('pveHealth.pinFix') };
}
// Registered desktops with their status, or null while the Host list is unknown.
function fleet(ctx) {
  const registered = registeredHostIds(ctx);
  return registered ? ctx.state.data.desktops.filter(d => registered.has(d.id) && (d.kind !== 'pc' || localPc(d))).map(d => ({ d, status: desktopStatus(ctx, d) })) : null;
}
const waitingRequests = ctx => (ctx.value('hostRequests')?.requests ?? []).filter(request => request.status === 'pending');
const go = (ctx, label, page, before = null, cls = 'secondary', id = null) => el('a', { href: ctx.hashFor(page, id), class: 'button ' + cls, onclick: event => { if (event.ctrlKey || event.metaKey || event.shiftKey) return; event.preventDefault?.(); before?.(); ctx.navigate(page, id); } }, label);
const desktopsView = (ctx, filter) => () => { ctx.state.ui.desktops = { ...(ctx.state.ui.desktops ?? { byNode: false }), query: '', filter }; };

// 지금 할 일: what the operator has to do now, most urgent first. Same-kind
// items are one group whose rows keep their own action; a group shows at most
// four rows and things that can wait are folded. Every action is an existing
// one (the request dialog, the power confirmation, a page link).
// One amber fill at most per row, and only where a person decides (확인하고
// 승인). A recommended action among several rows is the amber tint (tonal);
// rows of one group keep their buttons in the same places (more.pair).
const todoCap = 4;
// 지금 접속 중 on 홈 lists this many connections.
const liveRows = 6;
// The state of a row is a small dot before its title (red: nobody can connect, amber: someone decides); no bar.
// A row with nothing to mark keeps the dot's place empty, so every title starts on the same line.
const todoDot = tone => el('i', { class: 'todo-dot is-' + (tone === 'bad' || tone === 'warn' ? tone : 'none'), 'aria-hidden': 'true' });
function todoItem(tone, iconName, title, text, actions = []) {
  return el('div', { class: 'todo-item tone-' + tone }, el('span', { class: 'todo-icon' }, icon(iconName)), todoDot(tone),
    el('span', { class: 'todo-text' }, el('strong', { class: 'todo-title' }, title), text ? el('small', {}, text) : null),
    actions.filter(Boolean).length ? el('span', { class: 'todo-actions' }, actions) : null);
}
function todoGroup(key, tone, iconName, title, text, rows, more) {
  return el('details', { class: 'todo-group', open: true, 'data-keep-open': 'todo:' + key, 'data-todo': key },
    el('summary', { class: 'todo-item tone-' + tone }, el('span', { class: 'todo-icon' }, icon(iconName)), todoDot(tone),
      el('span', { class: 'todo-text' }, el('strong', { class: 'todo-title' }, title), text ? el('small', {}, text) : null), icon('chevron', 'icon todo-chevron')),
    el('div', { class: 'todo-rows' }, rows.slice(0, todoCap).map(([name, detail, actions]) => el('div', { class: 'todo-row' },
      el('span', { class: 'todo-text' }, el('strong', {}, name), detail ? el('small', {}, detail) : null), el('span', { class: 'todo-actions' + (more.pair ? ' is-pair' : '') }, actions)))),
    rows.length > todoCap ? el('p', { class: 'todo-more' }, t('overview.1', { value: rows.length - todoCap, unit: more.unit }), more.link) : null);
}
function requestRows(ctx, requests) {
  return requests.map(request => {
    // The VM the table knows, or the one the server found in Proxmox for a new VM.
    const vm = requestVm(request), one = !!vm;
    return [request.hostname, [request.verificationCode ? el('span', { class: 'todo-code' }, t('overview.2'), el('strong', { class: 'mono-value' }, request.verificationCode)) : t('overview.3'), one ? ` · ${vm.name} (VM ${vm.vmid})` : t('overview.4')],
      [one ? requestApproveButton(ctx, request, { label: t('common.111') }) : go(ctx, t('overview.5'), 'desktops', null, 'tonal'), button(t('common.114'), () => rejectHostRequest(ctx, request), 'quiet')]];
  });
}
function todoParts(ctx) {
  const { proxmox, assignments, hostUpdates } = ctx.state.data, update = ctx.state.serverInfo?.update;
  const known = fleet(ctx), states = known ?? [], requests = waitingRequests(ctx), eligible = hostUpdateEligible(ctx);
  const tokenDays = proxmox.configured ? daysLeft(proxmox.expiresAt) : null;
  const dayFailures = collect(ctx).filter(e => e.failure && !e.routine && !e.stats && Date.now() - e.time < 86400000);
  const now = [], later = [];
  // An unread Host list is named, never taken for an empty one.
  const hostsEntry = ctx.resource('hosts');
  if (!known && (hostsEntry?.error || hostsEntry?.value)) now.push(todoItem('bad', 'alert', t('overview.6'), hostsEntry.error || t('common.81'), [button(t('common.41'), () => ctx.load('hosts', { force: true }), 'tonal', 'refresh')]));
  // A refused certificate is named with its fix instead of the general connection error.
  const certificate = proxmoxCertificateNotice(proxmox);
  if (certificate?.failure) now.push(todoItem('bad', 'shield', certificate.title, certificate.text, [go(ctx, t('overview.9'), 'proxmox', null, 'tonal')]));
  else if (proxmox.configured && (proxmox.state === 'error' || ctx.state.pveError)) now.push(todoItem('bad', 'cube', t('overview.7'), messages[proxmox.errorCode] ?? ctx.state.pveError ?? t('overview.8'), [go(ctx, t('overview.9'), 'proxmox', null, 'tonal')]));
  // Desktops nobody can connect to (hosts.js desktopStatus): a Host that does
  // not answer, or one that reports its virtual display needs a restart or recovery.
  const users = id => assignments.filter(a => a.desktop_id === id).map(a => a.username).join(', ');
  const unreachable = states.filter(({ status }) => status.key === 'bad').map(({ d, status }) => {
    const display = status.cause !== 'silent', running = d.kind !== 'pc' && d.pve_power_status === 'running';
    // Two fixed places: 다시 시작 | 데스크톱 열기. The one that helps is tinted;
    // a desktop with no power control keeps the first place empty.
    const restart = running ? button(t('common.46'), () => powerForm(ctx, d, 'reboot'), display ? 'tonal' : 'secondary', 'refresh') : el('span', { class: 'todo-slot', 'aria-hidden': 'true' });
    return [d.name, [status.why, users(d.id)].filter(Boolean).join(' · '), [restart, ctx.link(t('common.136'), 'desktop', d.id, { class: 'button ' + (display && running ? 'secondary' : 'tonal') })]];
  });
  if (unreachable.length) now.push(todoGroup('unreachable', 'bad', 'alert', t('overview.10', { unreachableCount: unreachable.length }), null, unreachable, { unit: t('common.93'), pair: true, link: go(ctx, t('overview.12'), 'desktops', desktopsView(ctx, 'bad'), 'quiet') }));
  if (requests.length) now.push(todoGroup('requests', 'warn', 'link', t('overview.13', { requestsCount: requests.length }), t('overview.14'), requestRows(ctx, requests), { unit: t('overview.15'), link: go(ctx, t('overview.16'), 'desktops', null, 'quiet') }));
  if (update?.state === 'available' && update.latest) now.push(todoItem('info', 'server', t('overview.17', { version: update.latest.version }), t('overview.18'), [go(ctx, t('overview.19'), 'updates')]));
  if (eligible.length) now.push(todoItem('info', 'download', t('overview.20', { eligibleCount: eligible.length }), t('overview.21', { value: hostUpdates?.latest?.version ?? '' }), [go(ctx, t('overview.22'), 'desktops', desktopsView(ctx, 'update'))]));
  if (dayFailures.length) now.push(todoItem('warn', 'activity', t('overview.24', { dayFailuresCount: dayFailures.length }), null, [button(t('overview.26'), () => openLog(ctx, { days: 1 }), 'secondary')]));
  const token = tokenDays !== null && tokenDays <= 30 ? todoItem(tokenDays <= 7 ? 'bad' : 'info', 'key', tokenDays <= 0 ? t('common.18') : t('common.19', { tokenDays }), t('overview.27'), [go(ctx, t('overview.28'), 'proxmox')]) : null;
  if (token) (tokenDays <= 7 ? now : later).push(token);
  // The pinned certificate's expiry, like the token's.
  if (certificate && !certificate.failure) (certificate.days <= 7 ? now : later).push(todoItem(certificate.days <= 7 ? 'bad' : 'info', 'shield', certificate.title, certificate.text, [go(ctx, t('overview.9'), 'proxmox')]));
  if (proxmox.configured && proxmox.consolePrivilege === 'missing') later.push(todoItem('warn', 'shield', t('overview.29'), t('overview.30'), [go(ctx, t('overview.31'), 'proxmox')]));
  return { now, later, known: !!known };
}
// The number on the 홈 entry of the sidebar.
export function todoCount(ctx) { try { return todoParts(ctx).now.length; } catch { return 0; } }
function todoPanel(now, later, known = true) {
  return panel({ title: t('overview.32'), count: now.length || null, className: 'todo-panel' },
    now.length ? el('div', { class: 'todo-list' }, now) : !known ? loadingState(t('overview.33')) : el('div', { class: 'todo-calm' }, el('span', { class: 'todo-icon' }, icon('check')), el('span', { class: 'todo-text' }, el('strong', {}, t('overview.34')), el('small', {}, t('overview.35')))),
    later.length ? el('details', { class: 'todo-later', 'data-keep-open': 'todo:later' }, el('summary', {}, t('overview.36', { laterCount: later.length })), el('div', { class: 'todo-list' }, later)) : null);
}
// One quiet line: the server answered, its version, and the Proxmox connection.
export function healthLine(ctx) {
  const info = ctx.state.serverInfo, proxmox = ctx.state.data.proxmox;
  // A refused certificate (pve-health-v1) is an error even when the last check passed.
  const pve = !proxmox.configured ? ['', t('overview.37')] : proxmox.state === 'error' || ctx.state.pveError || proxmoxCertificateNotice(proxmox)?.failure ? ['bad', t('overview.7')] : proxmox.state === 'ok' ? ['good', t('overview.38')] : ['', t('overview.39')];
  return el('p', { class: 'health-line', role: 'status' },
    el('span', { class: info ? 'is-good' : '' }, el('i', { 'aria-hidden': 'true' }), info ? t('overview.40', { version: info.version }) : ctx.state.serverInfoLoading ? t('overview.41') : t('overview.42')),
    el('span', { class: pve[0] ? 'is-' + pve[0] : '' }, el('i', { 'aria-hidden': 'true' }), pve[1]),
    proxmox.configured && proxmox.checkedAt ? el('span', {}, t('overview.43', { relative: relative(proxmox.checkedAt) })) : null);
}

// 시작하기: the five steps to the first connection. The strip stays until the
// last one is done; on an install with no desktop and no assignment yet it is
// the whole page, with the current step's action in place.
function firstRun(ctx, states, requests) {
  const { users, assignments, proxmox } = ctx.state.data, hostsLoaded = !!states, enrolled = states?.length ?? 0;
  // Apps and Hosts need an address before anything can connect, so it is the first step.
  // Not claimed missing while unknown (not read yet, or the read failed); a Host already here had an address.
  const accessValue = ctx.value?.('serverAccess'), accessEntry = ctx.resource?.('serverAccess');
  const accessReady = !accessValue || !!accessValue.applied || enrolled > 0 || requests.length > 0;
  // Its first read is still out: the strip is not drawn yet (it started on a later step and jumped back to the first).
  const accessPending = !accessValue && accessEntry?.loading === true && !accessEntry.error && enrolled === 0 && requests.length === 0;
  const steps = [
    [t('common.12'), accessReady],
    [t('common.10'), proxmox.configured || enrolled > 0 || requests.length > 0],
    [t('overview.44'), enrolled > 0 || requests.length > 0],
    [t('overview.45'), enrolled > 0],
    [t('common.139'), users.length > 1 || assignments.length > 0],
    [t('common.140'), assignments.length > 0],
  ];
  if (!hostsLoaded || accessPending || steps.every(([, done]) => done)) return null;
  const now = steps.findIndex(([, done]) => !done);
  // Every step opens its explanation, the later ones too (they look like tabs, so they must do something).
  // The current step shows by default; another one stays until the steps move on.
  const ui = ctx.state.ui, chosen = ui.firstRunStep && ui.firstRunStep.now === now ? ui.firstRunStep.index : now;
  const show = index => { ui.firstRunStep = { index, now }; ctx.render(); };
  const strip = el('ol', { class: 'first-steps', 'aria-label': t('overview.46') }, steps.map(([label, done], index) => el('li', {
    class: [done ? 'is-done' : index === now ? 'is-now' : '', index === chosen ? 'is-shown' : ''].filter(Boolean).join(' '), 'aria-current': index === now ? 'step' : null,
    role: 'button', tabindex: '0', 'aria-pressed': String(index === chosen), title: t('overview.47', { label }),
    onclick: () => show(index), onkeydown: event => { if (event.key === 'Enter' || event.key === ' ') { event.preventDefault?.(); show(index); } } },
    el('i', { 'aria-hidden': 'true' }, done ? icon('check') : String(index + 1)), el('span', {}, label), done ? el('span', { class: 'visually-hidden' }, t('overview.48')) : null)));
  const guide = () => enrollmentGuide(ctx, null);
  const row = (iconName, title, text, actions) => el('div', { class: 'row-line' }, el('span', { class: 'tile' }, icon(iconName)), el('div', { class: 'row-text' }, el('strong', {}, title), el('small', {}, text)), el('div', { class: 'row-actions' }, actions));
  const addresses = [accessValue?.applied?.https?.enabled ? accessValue.applied.https.origin : null, ...(accessValue?.applied?.http?.enabled ? [accessValue.applied.http.origin, ...(accessValue.applied.http.extraOrigins ?? [])] : [])].filter(Boolean);
  const stepPanels = [
    () => panel({ title: t('overview.49') }, row('link', t('common.12'), t('overview.50'), [go(ctx, t('common.12'), 'server-access', null, 'primary')])),
    () => panel({ title: t('overview.51') }, row('cube', 'Proxmox VE', t('overview.52'), [go(ctx, t('common.10'), 'proxmox', null, 'primary')])),
    () => panel({ title: t('overview.54') }, row('download', 'Multitap Host', addresses.length ? t('overview.56', { join: addresses.join(t('overview.55')) }) : t('overview.57'), [button(t('common.98'), guide, 'primary', 'link')])),
    () => !requests.length ? panel({ title: t('overview.58') }, row('check', t('common.88'), t('overview.59'), [go(ctx, t('overview.60'), 'desktops', null, 'secondary')])) :
      panel({ title: t('overview.58'), actions: [el('span', { class: 'panel-note' }, t('overview.61', { requestsCount: requests.length }))] },
      el('div', { class: 'todo-rows' }, requestRows(ctx, requests).slice(0, todoCap).map(([name, detail, actions]) => el('div', { class: 'todo-row' }, el('span', { class: 'todo-text' }, el('strong', {}, name), el('small', {}, detail)), el('span', { class: 'todo-actions' }, actions)))),
      el('p', { class: 'note' }, icon('info'), el('span', {}, t('overview.62'))),
      el('details', { class: 'dt-more', 'data-keep-open': 'first-run-help' }, el('summary', {}, t('overview.63')), el('p', {}, t('overview.64', { origin: globalThis.location.origin })))),
    () => panel({ title: t('overview.65') }, row('user', t('common.7'), t('overview.66'), [button(t('common.139'), () => userForm(ctx), 'primary', 'plus')])),
    () => panel({ title: t('overview.67') }, row('monitor', t('common.35'), t('overview.68'), [go(ctx, t('common.141'), 'users', null, 'primary')])),
  ];
  const next = chosen === now && now === 3 && !steps[4][1] ? panel({}, row('user', t('overview.69'), t('overview.70'), [button(t('overview.71'), () => userForm(ctx), 'secondary small')])) : null;
  // A later step, opened early: what it needs first. 사용자 만들기 needs nothing.
  const waits = { 2: [0], 3: [2], 5: [3, 4] }[chosen]?.filter(index => !steps[index][1]) ?? [];
  const early = chosen !== now && waits.length ? el('p', { class: 'note first-step-wait' }, icon('info'), el('span', {}, t('overview.73', { map: waits.map(index => t('overview.72', { value: index + 1, value2: steps[index][0] })).join(', ') }))) : null;
  const shown = stepPanels[chosen]();
  if (early) shown.append(early);
  return { strip, panels: [shown, next], whole: enrolled === 0 && assignments.length === 0 };
}

export function render(ctx) {
  const { users } = ctx.state.data;
  const states = fleet(ctx), sessionsEntry = ctx.resource('sessions'), live = ctx.value('sessions')?.sessions ?? null;
  const requests = waitingRequests(ctx), start = firstRun(ctx, states, requests);
  const { now, later, known } = todoParts(ctx);
  const count = key => states.filter(({ status }) => status.key === key).length;
  const tile = (label, valueNode, node) => { node.setAttribute('class', 'stat-tile'); node.replaceChildren(el('strong', {}, valueNode), el('span', {}, label)); return node; };
  const stats = el('nav', { class: 'stat-tiles', 'aria-label': t('overview.74') },
    tile(t('overview.75'), states ? [String(count('ok') + count('live')), el('small', {}, ` / ${states.length}`)] : '—', ctx.link('', 'desktops')),
    tile(t('common.69'), live ? String(live.length) : '—', ctx.link('', 'monitoring')),
    tile(t('common.94'), states ? String(count('bad')) : '—', go(ctx, '', 'desktops', desktopsView(ctx, 'bad'), '')),
    tile(t('common.7'), String(users.length), ctx.link('', 'users')));
  const client = ctx.value('clientDiagnostics')?.events ?? [];
  const since = id => { const times = client.filter(row => row.sessionId === id && ['streamStart', 'streamReady'].includes(row.event)).map(row => Date.parse(row.time)).filter(Number.isFinite); return times.length ? sinceText(Math.min(...times)) : null; };
  // Everyone here is connected: who, where and since when (one still connecting says so in place of the time).
  const sessionPanel = panel({ title: t('common.69'), count: live?.length ?? null, className: 'live-panel', actions: [ctx.link(t('common.142'), 'monitoring', null, { class: 'button quiet small' })] },
    !live ? (sessionsEntry?.error ? emptyState(t('common.70'), sessionsEntry.error, { iconName: 'alert', compact: true, tone: 'is-error' }) : loadingState()) :
      live.length ? [board([{ label: t('common.7'), cls: 'col-who is-name' }, { label: t('common.5'), cls: 'col-desktop is-left' }, { label: t('common.143'), cls: 'col-since' }],
        live.slice(0, liveRows).map(s => { const d = ctx.desktopById(s.desktopId);
          return { cells: [s.account ? ctx.link(el('strong', {}, s.account), 'user', s.account) : t('common.71'), d ? ctx.link(d.name, 'desktop', d.id) : s.desktopId, s.ready ? since(s.sessionId) ?? '—' : el('span', { class: 'muted' }, t('common.72'))] }; }), { className: 'live-table' }),
        // The count above is of every connection: when the list shows fewer, it says so (모두 보기 has the rest).
        live.length > liveRows ? el('p', { class: 'res-more live-more' }, t('overview.liveMoreF3', { total: live.length, shown: liveRows })) : null]
        : el('p', { class: 'panel-empty' }, t('overview.76')));
  const refresh = button(t('common.42'), async () => { await ctx.refresh(); for (const name of ['sessions', 'hosts', 'clientDiagnostics', 'hostRequests', 'resources']) void ctx.load(name, { force: true }); void ctx.loadServerInfo(); }, 'secondary', 'refresh');
  if (start?.whole) return [ctx.pageHeader({ title: t('overview.77'), description: t('overview.78'), actions: [refresh] }),
    start.strip, start.panels, now.some(node => !node.matches?.('[data-todo=requests]')) ? todoPanel(now.filter(node => !node.matches?.('[data-todo=requests]')), later) : null, healthLine(ctx)];
  // The numbers in a row of tiles, then what to do now, what the server and the desktops use (resources.js), who is connected, and the one line about the server (operations.css).
  return [ctx.pageHeader({ title: t('common.1'), actions: [refresh] }),
    stats, start ? [start.strip, start.panels] : null, todoPanel(now, later, known), resourcePanel(ctx), sessionPanel, healthLine(ctx)];
}
