// 기록: one page for finding out why something went wrong. 문제 찾기 (failed
// connections first, every record on request), 화질·속도 (streaming statistics)
// and 관리 작업 (the audit trail). Who is connected now, and who used what for
// how long, is 모니터링 (usage.js).
import { el, icon, button, stateDot, panel, emptyState, loadingState, errorState, select, segmented, queryBox, queryField, queryText, conditions, detailWindow } from './dom.js';
import { collect, logBoard, logWindow } from './timeline.js';
import { sessionHeadline, metricRow, sessionDetail } from './perf.js';
import { t } from './i18n.js';

export const logTabs = ['problems', 'quality', 'admin'];
const tabOf = route => logTabs.includes(route?.id) ? route.id : logTabs[0];
export const logNeeds = route => ({ problems: ['clientDiagnostics', 'hosts', 'sessions'], quality: ['sessions', 'streamStats'], admin: ['sessions'] })[tabOf(route)];
const logUi = ctx => ctx.state.ui.log ??= { query: '', source: 'all', vm: 'all', account: 'all', days: 7, failures: true, stats: false, routine: false, adminRoutine: false, perfVm: 'all', perfAccount: 'all', adminQuery: '' };
// Opens 기록 for one desktop or account (the links "기록에서 보기").
export function openLog(ctx, patch = {}, tab = null) {
  Object.assign(logUi(ctx), { query: '', source: 'all', vm: 'all', account: 'all', days: 7, failures: true, ...patch });
  ctx.navigate('log', tab);
}

const vmOrder = (a, b) => Number(a.pve_vmid ?? Infinity) - Number(b.pve_vmid ?? Infinity) || String(a.name).localeCompare(String(b.name), 'ko');
const desktopText = d => d.kind === 'pc' ? t('common.6', { name: d.name }) : `VM ${d.pve_vmid} · ${d.name}`;
const vmLabel = (ctx, id) => { const d = ctx.desktopById(id); return d ? desktopText(d) : id ?? t('monitor.1'); };
const toggle = (label, checked, onChange, key) => el('label', { class: 'toggle' }, el('input', { type: 'checkbox', checked, 'data-focus': key, onchange: event => onChange(event.target.checked === true) }), el('span', {}, label));
function resourceState(ctx, name, what) {
  const entry = ctx.resource(name);
  if (entry?.value) return null;
  return entry?.error ? errorState(t('monitor.2', { what }), entry.error, () => ctx.load(name, { force: true })) : loadingState(t('monitor.3', { what }));
}
// A record opens in a window over the list (timeline.js logWindow); the list keeps which one.
const showEntry = ctx => entry => { logUi(ctx).open = entry.key; ctx.render(); };
function opened(ctx, all) {
  const ui = logUi(ctx), entry = ui.open ? all.find(e => e.key === ui.open) : null;
  return entry ? logWindow(ctx, entry, () => { ui.open = null; ctx.render({ refocus: { key: 'log-open:' + entry.key } }); }) : null;
}
const keepNote = () => el('p', { class: 'page-note' }, icon('info'), t('monitor.4'));

// 문제 찾기: failed connections with their cause and fix; 전체 shows every
// record of every source with the source, statistics and bookkeeping filters.
function problems(ctx) {
  const ui = logUi(ctx), all = collect(ctx), q = ui.query.trim().toLowerCase(), since = Date.now() - ui.days * 86400000, show = showEntry(ctx);
  const entries = all.filter(e => e.time >= since && (ui.failures ? e.failure : (ui.source === 'all' || e.source === ui.source) && (ui.stats || !e.stats) && (ui.routine || !e.routine))
    && (ui.vm === 'all' || e.desktopId === ui.vm) && (ui.account === 'all' || e.account === ui.account || e.actor === ui.account)
    && (!q || [e.title, e.detail.join(' '), e.code, e.codeLabel, e.account, e.actor, e.desktopId, ctx.desktopById(e.desktopId)?.name].filter(Boolean).join(' ').toLowerCase().includes(q)));
  const failures = entries.filter(e => e.failure).length;
  // A person, a desktop or a search text is chosen: an empty list is then about them, not about the period.
  const narrowed = ui.account !== 'all' || ui.vm !== 'all' || !!q;
  const accounts = [...new Set([...ctx.state.data.users.map(u => u.username), ...all.map(e => e.account).filter(Boolean)])].sort((a, b) => a.localeCompare(b, 'ko'));
  const failed = ['clientDiagnostics', 'hosts'].map(name => [name, ctx.resource(name)]).filter(([, entry]) => entry?.error);
  // 조회 조건: who, which desktop, how far back and a search text; with 전체 also the source and the two kinds of
  // record that are left out. They change the list when 조회 is pressed. 문제만 / 전체 is the view and acts at once.
  const terms = conditions(ui, ['account', 'vm', 'days', 'query', 'source', 'stats', 'routine'], { account: 'all', vm: 'all', days: 7, query: '', source: 'all', stats: false, routine: false }), draft = terms.values();
  const apply = (refocus = { key: 'query-apply' }) => { terms.apply(); ctx.render({ refocus }); };
  const filters = queryBox(t('monitor.5'), [
    queryField(t('common.7'), select(t('monitor.6'), [['all', t('monitor.7')], ...accounts.map(a => [a, a])], draft.account, v => terms.set({ account: v }), { attrs: { 'data-focus': 'activity-account' } })),
    queryField(t('common.5'), select(t('monitor.8'), [['all', t('monitor.7')], ...[...ctx.state.data.desktops].sort(vmOrder).map(d => [d.id, desktopText(d)])], draft.vm, v => terms.set({ vm: v }), { attrs: { 'data-focus': 'activity-vm' } })),
    queryField(t('common.127'), select(t('common.127'), [[1, t('monitor.9')], [2, t('monitor.10')], [7, t('monitor.11')]], draft.days, v => terms.set({ days: Number(v) || 7 }), { attrs: { 'data-focus': 'activity-days' } })),
    ...(ui.failures ? [] : [
      queryField(t('monitor.12'), select(t('monitor.12'), [['all', t('monitor.13')], ['audit', t('common.85')], ['client', t('common.128')], ['host', 'Host']], draft.source, v => terms.set({ source: v }), { attrs: { 'data-focus': 'activity-source' } })),
      toggle(t('monitor.14'), draft.stats, v => terms.set({ stats: v }), 'activity-stats'),
      toggle(t('monitor.15'), draft.routine, v => terms.set({ routine: v }), 'activity-routine')]),
    queryField(t('common.89'), queryText({ key: 'activity-search', value: draft.query, placeholder: t('monitor.16'), onInput: v => terms.set({ query: v }), onApply: apply }), 'is-search')],
    apply, () => { terms.clear(); ctx.render(); });
  return [filters, panel({ className: 'activity-panel' },
    el('div', { class: 'result-bar' }, el('span', {}, t('monitor.17'), el('strong', {}, t('monitor.18', { entriesCount: entries.length })), !ui.failures && failures ? el('span', { class: 'is-bad' }, t('monitor.19', { failures })) : null),
      !ctx.value('clientDiagnostics') && !ctx.resource('clientDiagnostics')?.error ? el('small', { class: 'muted' }, t('monitor.20')) : el('small', { class: 'muted' }, t('monitor.21')),
      segmented(t('common.129'), [['problems', t('monitor.22')], ['everything', t('monitor.7')]], ui.failures ? 'problems' : 'everything', v => { ui.failures = v === 'problems'; ctx.render({ refocus: { key: 'segment-' + v } }); })),
    failed.map(([name, entry]) => el('p', { class: 'is-error' }, t('monitor.23', { value: name === 'hosts' ? 'Host' : t('common.128'), error: entry.error }))),
    logBoard(ctx, entries, { limit: 200, cause: ui.failures, open: show, emptyTitle: narrowed && all.length ? t('common.130') : ui.failures ? t('monitor.24') : all.length ? t('common.130') : t('common.40'),
      emptyText: narrowed && all.length ? t('monitor.25') : ui.failures ? t('monitor.26') : all.length ? t('monitor.27') : t('monitor.28') })),
    keepNote(), opened(ctx, all)];
}

// 화질·속도: streaming statistics per session.
function quality(ctx) {
  const ui = logUi(ctx), sessions = ctx.value('sessions')?.sessions ?? [], stats = ctx.value('streamStats')?.sessions ?? [];
  const vmOptions = [...new Set(stats.map(s => s.desktopId).filter(Boolean))].map(id => ctx.desktopById(id) ?? { id, pve_vmid: id, name: '' }).sort(vmOrder);
  const accounts = [...new Set(stats.map(s => s.account).filter(Boolean))].sort((a, b) => a.localeCompare(b, 'ko'));
  const shown = stats.filter(s => (ui.perfVm === 'all' || s.desktopId === ui.perfVm) && (ui.perfAccount === 'all' || s.account === ui.perfAccount));
  const liveIds = new Set(sessions.map(s => s.sessionId)), statsEntry = ctx.value('streamStats');
  const terms = conditions(ui, ['perfVm', 'perfAccount'], { perfVm: 'all', perfAccount: 'all' }), draft = terms.values();
  const filters = queryBox(t('monitor.29'), [
    queryField(t('common.5'), select(t('monitor.8'), [['all', t('monitor.7')], ...vmOptions.map(d => [d.id, d.name ? desktopText(d) : d.id])], draft.perfVm, v => terms.set({ perfVm: v }), { attrs: { 'data-focus': 'perf-vm' } })),
    queryField(t('common.7'), select(t('monitor.6'), [['all', t('monitor.7')], ...accounts.map(a => [a, a])], draft.perfAccount, v => terms.set({ perfAccount: v }), { attrs: { 'data-focus': 'perf-account' } }))],
    () => { terms.apply(); ctx.render({ refocus: { key: 'query-apply' } }); }, () => { terms.clear(); ctx.render(); });
  // One connection is a row with its numbers; 상세 opens its charts and table in a window.
  const keyOf = (session, index) => String(session.sessionId ?? index), opened = shown.map((session, index) => [session, keyOf(session, index)]).find(([, key]) => key === ui.perfOpen) ?? null;
  const show = key => { ui.perfOpen = key; ctx.render(); };
  const detail = opened ? (() => { const [session, key] = opened, head = sessionHeadline(session, ctx.desktopById(session.desktopId));
    return detailWindow(ctx, { key: 'perf:' + key, kind: t('monitor.30'), title: head.title, sub: head.meta, className: 'perf-window', focusKey: 'perf-open-' + key,
      badges: liveIds.has(session.sessionId) ? stateDot(t('common.49'), 'green') : null, close: () => { ui.perfOpen = null; ctx.render({ refocus: { key: 'perf-open-' + key } }); },
      tabs: [{ key: 'detail', label: t('monitor.30'), nodes: [metricRow(head.s), sessionDetail(session)] }] }); })() : null;
  return [resourceState(ctx, 'streamStats', t('monitor.31')) ? null : filters, panel({ title: t('monitor.30'), count: shown.length, description: t('monitor.32') },
    resourceState(ctx, 'streamStats', t('monitor.31')) ?? [
      shown.length ? el('ul', { class: 'perf-list' }, shown.map((session, index) => {
        const head = sessionHeadline(session, ctx.desktopById(session.desktopId)), key = keyOf(session, index);
        return el('li', { class: 'perf-session' },
          el('div', { class: 'perf-row' }, el('span', { class: 'perf-title' }, el('strong', {}, head.title), liveIds.has(session.sessionId) ? stateDot(t('common.49'), 'green') : null, el('small', {}, head.meta)),
            button(t('common.91'), () => show(key), 'secondary small', null, { 'aria-label': t('common.131', { title: head.title }), 'data-focus': 'perf-open-' + key })),
          metricRow(head.s));
      })) : emptyState(stats.length ? t('monitor.33') : t('monitor.34'), stats.length ? t('monitor.35') : t('monitor.36'), { iconName: 'activity', compact: true }),
      el('p', { class: 'field-hint' }, t('monitor.38', { value: statsEntry?.limit ?? 480, value2: statsEntry?.truncated ? t('monitor.37') : '' }))]), detail];
}

// 관리 작업: what administrators and the server did (the audit trail).
function adminWork(ctx) {
  const ui = logUi(ctx), q = ui.adminQuery.trim().toLowerCase();
  const all = collect(ctx, { clientRows: [], hostList: [] }), show = showEntry(ctx);
  const entries = all.filter(e => (ui.adminRoutine || !e.routine) && (!q || [e.title, e.detail.join(' '), e.code, e.account, e.actor, ctx.desktopById(e.desktopId)?.name].filter(Boolean).join(' ').toLowerCase().includes(q)));
  const terms = conditions(ui, ['adminQuery', 'adminRoutine'], { adminQuery: '', adminRoutine: false }), draft = terms.values();
  const apply = (refocus = { key: 'query-apply' }) => { terms.apply(); ctx.render({ refocus }); };
  return [queryBox(t('monitor.39'), [
      toggle(t('monitor.15'), draft.adminRoutine, v => terms.set({ adminRoutine: v }), 'admin-routine'),
      queryField(t('common.89'), queryText({ key: 'admin-search', value: draft.adminQuery, placeholder: t('monitor.40'), onInput: v => terms.set({ adminQuery: v }), onApply: apply }), 'is-search')],
      apply, () => { terms.clear(); ctx.render(); }),
    panel({ className: 'activity-panel' },
    el('div', { class: 'result-bar' }, el('span', {}, t('monitor.17'), el('strong', {}, t('monitor.18', { entriesCount: entries.length }))), el('small', { class: 'muted' }, t('monitor.41'))),
    logBoard(ctx, entries, { limit: 200, cause: false, open: show, emptyTitle: all.length ? t('common.130') : t('monitor.42'), emptyText: all.length ? t('common.132') : t('monitor.43') })), opened(ctx, all)];
}

export function log(ctx, route) {
  const tab = tabOf(route);
  const tabs = [['problems', t('monitor.44')], ['quality', t('monitor.30')], ['admin', t('monitor.45')]];
  const pick = key => { logUi(ctx).open = null; ctx.navigate('log', key === logTabs[0] ? null : key); ctx.render({ refocus: { key: 'log-tab-' + key } }); };
  const bar = el('div', { class: 'log-tabs', role: 'tablist', 'aria-label': t('monitor.46') }, tabs.map(([key, title], index) =>
    button(title, () => pick(key), 'log-tab' + (tab === key ? ' selected' : ''), null, { id: 'log-tab-' + key, role: 'tab', 'aria-selected': String(tab === key), 'aria-controls': 'log-panel', tabindex: tab === key ? '0' : '-1', 'data-focus': 'log-tab-' + key,
      onkeydown: event => { const delta = event.key === 'ArrowRight' ? 1 : event.key === 'ArrowLeft' ? -1 : null; if (delta !== null || ['Home', 'End'].includes(event.key)) { event.preventDefault(); pick(tabs[event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : (index + delta + tabs.length) % tabs.length][0]); } } })));
  const refresh = button(t('common.42'), async () => { await ctx.refresh(); for (const name of logNeeds(route)) void ctx.load(name, { force: true }); }, 'secondary', 'refresh');
  return [ctx.pageHeader({ title: t('common.2'), actions: [refresh] }), bar,
    el('div', { id: 'log-panel', class: 'log-panel', role: 'tabpanel', 'aria-labelledby': 'log-tab-' + tab }, { problems, quality, admin: adminWork }[tab](ctx))];
}
