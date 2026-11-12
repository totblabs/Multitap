// 모니터링: who is signed in and connected now, how long each person and each
// desktop was connected over a range of days, and one day's sign-ins and
// connections. The server knows connected time, not work: every number here
// is 연결 시간, and the page says that time away from the keyboard is in it.
// Data: /usage/live, /usage/summary and /usage/events (server/src/usage.mjs);
// days are the server's local days and the page names its time zone.
import { el, icon, button, stateDot, table, panel, emptyState, loadingState, errorState, segmented, select, date, hhmm, sinceText, fixed, integer, detailWindow } from './dom.js';
import { rankBars, dayBars } from './charts.js';
import { registeredHostIds } from './hosts.js';
import { t, lang } from './i18n.js';

export const usageTabs = ['now', 'users', 'desktops', 'history'];
const tabOf = route => usageTabs.includes(route?.id) ? route.id : usageTabs[0];
const pad = value => String(value).padStart(2, '0');
const dayPattern = /^\d{4}-\d\d-\d\d$/;
const localDay = (now = Date.now()) => { const d = new Date(now); return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`; };
// Calendar arithmetic on YYYY-MM-DD (no time zone involved).
const dayNumber = day => { const [y, m, d] = day.split('-').map(Number); return Date.UTC(y, m - 1, d) / 86400000; };
const shiftDay = (day, count) => new Date((dayNumber(day) + count) * 86400000).toISOString().slice(0, 10);
// One letter per weekday in Korean (일…토); English uses the short name (Sun…Sat).
const weekdayNames = new Intl.DateTimeFormat('en-US', { weekday: 'short', timeZone: 'UTC' });
const weekday = day => lang === 'ko' ? t('usage.1')[new Date(dayNumber(day) * 86400000).getUTCDay()] : weekdayNames.format(new Date(dayNumber(day) * 86400000));
const dayLabel = day => `${day} (${weekday(day)})`;
const shortDay = day => { const [, m, d] = day.split('-').map(Number); return t('usage.2', { m, d, weekday: weekday(day) }); };
// 연결 시간 as text: whole minutes, never rounded up to the next hour.
export function connectedText(ms) {
  if (!Number.isFinite(ms) || ms < 0) return '—';
  if (ms < 60000) return ms > 0 ? t('common.145') : t('usage.3');
  const hours = Math.floor(ms / 3600000), minutes = Math.floor(ms % 3600000 / 60000);
  return hours ? (minutes ? t('usage.4', { hours, minutes }) : t('usage.5', { hours })) : t('common.101', { minutes });
}
// The top of a day chart: a round number of minutes or hours.
const niceDuration = ms => ([10, 20, 30, 60, 120, 180, 240, 360, 480, 720, 960, 1440].find(minutes => minutes * 60000 >= ms) ?? Math.ceil(ms / 21600000) * 360) * 60000;
const platforms = { windows: 'Windows', android: 'Android', web: t('common.133') };
const clientText = row => [platforms[row.platform] ?? null, row.version ? row.version.replace(/\+\d+$/, '') : null].filter(Boolean).join(' · ') || t('common.121');
const endReasons = { logout: t('usage.6'), session_idle_timeout: t('usage.7'), pc_login_replaced: t('usage.8'), expired: t('usage.9'), ended: t('usage.10'), client_stopped: t('common.125'),
  client_disconnected: t('usage.11'), client_timeout: t('usage.12'), client_error: t('usage.13'), taken_over: t('usage.14'), server_stopping: t('usage.15'), server_restart: t('usage.16'),
  authorization_revoked: t('usage.17'), host_disconnected: t('usage.18'), host_revoked: t('usage.19'), host_maintenance: t('usage.20'), unknown: t('common.121') };
const reasonText = event => event.live ? t('usage.21') : event.endReason ? endReasons[event.endReason] ?? t('usage.22') : event.endedAt ? t('usage.23') : t('usage.24');

const usageUi = ctx => ctx.state.ui.usage ??= { range: '7', from: '', to: '', user: null, desktop: null, day: null, who: 'all', query: '', pages: 1, table: false };
const today = ctx => { const value = ctx.value('usageLive')?.today?.date; return dayPattern.test(value ?? '') ? value : localDay(); };
// The chosen range as [from, to] (inclusive days).
function span(ctx) {
  const ui = usageUi(ctx), end = today(ctx);
  if (ui.range === 'custom' && dayPattern.test(ui.from) && dayPattern.test(ui.to) && ui.from <= ui.to && dayNumber(ui.to) - dayNumber(ui.from) < 366) return [ui.from, ui.to];
  return [shiftDay(end, 1 - ({ 1: 1, 30: 30 }[ui.range] ?? 7)), end];
}
const eventsName = (from, to, kind, subject, page) => `usageEvents:${from}/${to}${kind ? `/${kind}/${encodeURIComponent(subject)}` : ''}${page > 1 ? `/page/${page}` : ''}`;
function eventNames(ctx, tab) {
  const ui = usageUi(ctx), pages = Array.from({ length: Math.max(1, Math.min(50, ui.pages)) }, (_, index) => index + 1);
  // One person's day also keeps the whole day's first page: the per-person counts come from it.
  if (tab === 'history') { const day = ui.day ?? today(ctx); return [...(ui.who === 'all' ? [] : [eventsName(day, day, null, null, 1)]), ...pages.map(page => eventsName(day, day, ui.who === 'all' ? null : 'user', ui.who, page))]; }
  const subject = tab === 'users' ? ui.user : tab === 'desktops' ? ui.desktop : null, [from, to] = span(ctx);
  return subject ? pages.map(page => eventsName(from, to, tab === 'users' ? 'user' : 'desktop', subject, page)) : [];
}
export function usageNeeds(ctx, route) {
  const tab = tabOf(route), names = ['usageLive'];
  if (tab === 'now') names.push('streamStats');
  if (tab === 'users' || tab === 'desktops') names.push('usageSummary:' + span(ctx).join('/'));
  return [...names, ...eventNames(ctx, tab)];
}
// Opens 모니터링 on one tab, optionally on one person, desktop or day.
export function openUsage(ctx, tab = null, patch = {}) {
  Object.assign(usageUi(ctx), { pages: 1, ...patch });
  ctx.navigate('monitoring', tab && tab !== usageTabs[0] ? tab : null);
}
// A changed choice: draw it, then read what it needs. Ranges and days looked
// at earlier are let go, the newest dozen aside, so a long look through the
// calendar does not pile up in the page.
function apply(ctx, route, refocus = null) {
  ctx.render(refocus ? { refocus: { key: refocus } } : {});
  const needs = usageNeeds(ctx, route), resources = ctx.state.resources;
  if (resources) {
    const old = [...resources.keys()].filter(name => /^usage(?:Events|Summary):/.test(name) && !needs.includes(name));
    for (const name of old.slice(0, Math.max(0, old.length - 12))) resources.delete(name);
  }
  for (const name of needs) void ctx.load(name);
}
function waiting(ctx, name, what) {
  const entry = ctx.resource(name);
  if (entry?.value) return null;
  return entry?.error ? errorState(t('usage.25', { what }), entry.error, () => ctx.load(name, { force: true })) : loadingState(t('usage.26', { what }));
}
const userLink = (ctx, account) => account ? (ctx.userByName(account) ? ctx.link(account, 'user', account, { class: 'row-link' }) : account) : t('common.71');
const desktopLink = (ctx, row) => ctx.desktopById(row.desktopId) ? ctx.link(row.desktopName ?? row.desktopId, 'desktop', row.desktopId) : row.desktopName ?? row.desktopId ?? '—';
const address = value => value ? el('span', { class: 'mono-value' }, value) : el('span', { class: 'muted' }, t('common.38'));
const since = value => { const time = Date.parse(value); return Number.isFinite(time) ? el('time', { datetime: value, title: date(time) }, sinceText(time)) : el('span', { class: 'muted' }, t('common.38')); };

// 지금: the connections this server relays and the sign-ins that are valid now.
function now(ctx) {
  const live = ctx.value('usageLive'), stats = ctx.value('streamStats')?.sessions ?? [];
  const state = waiting(ctx, 'usageLive', t('usage.28'));
  if (state) return [panel({ title: t('common.69') }, state)];
  const quality = id => { const w = stats.find(s => s.sessionId === id)?.windows.at(-1)?.values; const fps = w?.textureFps10 ?? w?.decodeFps10;
    return w ? ctx.link(`${Number.isFinite(fps) ? fixed(fps / 10) + ' fps' : '—'} · ${Number.isFinite(w.rttMs) ? integer(w.rttMs) + ' ms' : '—'}`, 'log', 'quality') : null; };
  const connections = live.connections.map(row => [userLink(ctx, row.account), desktopLink(ctx, row), clientText(row), address(row.address), since(row.since),
    // Only what was observed: a stream's last reported window, or that none came yet (준비 중 while it connects).
    !row.ready ? el('span', { class: 'muted' }, t('common.72')) : quality(row.sessionId) ?? el('span', { class: 'muted' }, '—')]);
  const logins = live.logins.map(row => [userLink(ctx, row.account), clientText(row), address(row.address), since(row.since),
    row.connections ? t('usage.29', { connections: row.connections }) : el('span', { class: 'muted' }, t('usage.30'))]);
  return [
    panel({ title: t('common.69'), count: live.connections.length },
      table([t('common.7'), t('common.5'), t('common.128'), t('usage.32'), t('common.143'), t('usage.33')], connections, t('usage.34'), t('usage.35'), { className: 'usage-live' })),
    panel({ title: t('usage.36'), count: live.logins.length, description: t('usage.37') },
      table([t('common.7'), t('common.128'), t('usage.32'), t('usage.38'), t('usage.39')], logins, t('common.134'), t('usage.40'), { className: 'usage-logins' }))];
}

// One list of sign-ins and connections (기록, and under a chosen person or desktop).
function eventTable(ctx, events, { people = true, desktops = true, dated = false, emptyTitle, emptyText }) {
  const clock = value => { const time = Date.parse(value); return Number.isFinite(time) ? hhmm(time) : '—'; };
  const headers = [t('usage.41'), t('common.62'), ...(people ? [t('common.7')] : []), ...(desktops ? [t('common.5')] : []), t('usage.42'), t('usage.43'), t('usage.44')];
  const rows = events.map(event => {
    const start = Date.parse(event.startedAt), sameDay = !event.endedAt || localDay(start) === localDay(Date.parse(event.endedAt));
    const length = event.kind === 'login' ? (event.durationMs === null ? el('span', { class: 'muted' }, '—') : el('span', { class: 'muted', title: t('usage.45') }, connectedText(event.durationMs)))
      : [el('strong', {}, connectedText(event.durationMs)), event.estimated ? el('small', { class: 'usage-estimated', title: t('usage.47') }, t('usage.46')) : null];
    // A list over several days names each row's day.
    return [el('time', { datetime: event.startedAt, title: date(start) }, dated ? el('small', {}, shortDay(localDay(start))) : null, `${clock(event.startedAt)} ~ ${event.live ? t('common.65') : event.endedAt ? (sameDay ? clock(event.endedAt) : date(event.endedAt)) : '?'}`),
      event.kind === 'login' ? t('common.16') : t('usage.39'), ...(people ? [userLink(ctx, event.account)] : []),
      ...(desktops ? [event.kind === 'connection' ? desktopLink(ctx, event) : el('span', { class: 'muted' }, '—')] : []),
      el('span', { class: 'usage-client' }, event.source === 'audit' ? el('span', { class: 'muted' }, t('usage.48')) : clientText(event), event.address ? el('small', { class: 'mono-value' }, event.address) : null), length,
      event.live ? stateDot(t('usage.21'), 'green') : reasonText(event)];
  });
  return table(headers, rows, emptyTitle, emptyText, { className: 'usage-events' });
}
// The pages of one event list read so far: { events, more, counts, accounts } or a state node.
function eventPages(ctx, names) {
  const first = waiting(ctx, names[0], t('usage.49'));
  if (first) return { state: first };
  const pages = names.map(name => ctx.value(name)).filter(Boolean);
  return { events: pages.flatMap(page => page.events), more: pages.length === names.length && pages.at(-1).more === true, loading: pages.length < names.length, counts: pages[0].counts, accounts: pages[0].accounts };
}
const moreButton = (ctx, route, list) => list.more || list.loading ? el('div', { class: 'usage-more' }, button(list.loading ? t('usage.50') : t('usage.51'), () => { usageUi(ctx).pages++; apply(ctx, route, 'usage-more'); }, 'secondary', null, { 'data-focus': 'usage-more', disabled: list.loading })) : null;

function rangeControls(ctx, route) {
  const ui = usageUi(ctx), end = today(ctx), [from, to] = span(ctx);
  const pick = value => { ui.range = value; ui.pages = 1; if (value === 'custom' && !dayPattern.test(ui.from)) { ui.from = from; ui.to = to; } apply(ctx, route, 'segment-' + value); };
  const field = (label, key, value) => el('label', { class: 'usage-date' }, el('span', {}, label), el('input', { type: 'date', value, min: shiftDay(end, -730), max: end, 'data-focus': 'usage-' + key, 'aria-label': label,
    onchange: event => { if (!dayPattern.test(event.target.value)) return; ui[key] = event.target.value; if (ui.from > ui.to) ui[key === 'from' ? 'to' : 'from'] = event.target.value; ui.pages = 1; apply(ctx, route, 'usage-' + key); } }));
  return el('div', { class: 'query-box usage-range', role: 'group', 'aria-label': t('usage.52') }, el('span', { class: 'query-name' }, t('common.127')), segmented(t('common.127'), [['1', t('usage.53')], ['7', t('usage.54')], ['30', t('usage.55')], ['custom', t('usage.56')]], ui.range, pick),
    ui.range === 'custom' ? [field(t('common.150'), 'from', from), field(t('usage.44'), 'to', to), ui.from && dayNumber(ui.to) - dayNumber(ui.from) >= 366 ? el('small', { class: 'is-error' }, t('usage.57')) : null] : el('small', { class: 'muted' }, from === to ? dayLabel(from) : `${dayLabel(from)} ~ ${dayLabel(to)}`));
}

// 사용자별 · 데스크톱별: the comparison, then the chosen one's days and connections.
function ranking(ctx, route, kind) {
  const ui = usageUi(ctx), [from, to] = span(ctx), name = `usageSummary:${from}/${to}`, people = kind === 'users';
  const state = waiting(ctx, name, t('usage.58')), data = ctx.value(name);
  const title = people ? t('usage.59') : t('usage.60');
  if (state) return [rangeControls(ctx, route), panel({ title, className: 'usage-ranking' }, state)];
  const items = (people ? data.users : data.desktops).map(item => ({ ...item, key: people ? item.account : item.desktopId, name: people ? item.account ?? t('common.71') : ctx.desktopById(item.desktopId)?.name ?? item.desktopName }));
  const chosenKey = people ? ui.user : ui.desktop, chosen = items.find(item => item.key === chosenKey) ?? null;
  const choose = key => { ui[people ? 'user' : 'desktop'] = chosenKey === key ? null : key; ui.pages = 1; apply(ctx, route, `usage-${kind}-${key}`); };
  const note = item => [t('usage.61', { connections: item.connections }), people ? t('usage.62', { logins: item.logins }) : t('usage.63', { usersCount: item.users.length })].join(' · ');
  const shown = items.slice(0, 50);
  const view = ui.table
    ? table([people ? t('common.7') : t('common.5'), t('usage.43'), t('usage.39'), people ? t('common.16') : t('usage.64'), t('usage.65'), t('common.39')], items.map(item => [
      el('button', { type: 'button', class: 'link-button', 'data-focus': `usage-${kind}-${item.key}`, 'aria-pressed': String(item.key === chosenKey), onclick: () => choose(item.key) }, item.name), connectedText(item.connectedMs), t('usage.66', { connections: item.connections }),
      people ? t('usage.67', { logins: item.logins }) : item.users.map(user => user.account ?? t('common.71')).join(', '), item.firstAt ? date(item.firstAt) : '—', item.lastAt ? date(item.lastAt) : '—']), t('common.40'), '', { className: 'usage-summary' })
    : shown.length ? [rankBars({ label: title, rows: shown.map(item => ({ key: item.key, name: item.name, value: item.connectedMs, note: note(item) })), format: connectedText, selected: chosenKey, onSelect: choose, focusKey: 'usage-' + kind }),
      items.length > shown.length ? el('p', { class: 'field-hint' }, t('usage.68', { shownCount: shown.length, value: people ? t('common.105') : t('common.93') })) : null]
      : emptyState(t('common.40'), t('usage.71', { value: people ? t('usage.69') : t('usage.70') }), { compact: true, iconName: 'activity' });
  const head = panel({ title, count: items.length, description: t('usage.73', { connectedText: connectedText(data.totals.connectedMs), connections: data.totals.connections, value: people ? t('usage.72', { logins: data.totals.logins }) : '' }),
    actions: [segmented(t('common.129'), [['chart', t('usage.74')], ['table', t('usage.75')]], ui.table ? 'table' : 'chart', value => { ui.table = value === 'table'; ctx.render({ refocus: { key: 'segment-' + value } }); })], className: 'usage-ranking' },
    view,
    data.truncated ? el('p', { class: 'is-error' }, t('usage.76')) : null,
    !chosen && items.length ? el('p', { class: 'field-hint' }, t('usage.77', { value: people ? t('common.60') : t('common.5') })) : null);
  return [rangeControls(ctx, route), head, chosen ? detail(ctx, route, kind, chosen, data) : null];
}
function detail(ctx, route, kind, item, data) {
  const ui = usageUi(ctx), people = kind === 'users', list = eventPages(ctx, eventNames(ctx, kind));
  const days = data.days.map((day, index) => ({ label: shortDay(day), value: item.daily[index], note: t('usage.78', { value: item.dailyConnections[index] }) }));
  const used = item.daily.filter(value => value > 0).length;
  const numbers = el('dl', { class: 'usage-facts' }, [[t('usage.43'), connectedText(item.connectedMs)], [t('usage.39'), t('usage.66', { connections: item.connections })], people ? [t('common.16'), t('usage.67', { logins: item.logins })] : [t('usage.64'), t('usage.63', { usersCount: item.users.length })], [t('usage.79'), t('usage.80', { used, daysCount: data.days.length })],
    [t('usage.65'), item.firstAt ? date(item.firstAt) : '—'], [t('common.39'), item.lastAt ? date(item.lastAt) : '—']].map(([label, value]) => el('div', {}, el('dt', {}, label), el('dd', {}, value))));
  const trend = data.days.length < 2 ? null : ui.table
    ? table([t('usage.81'), t('usage.43'), t('usage.39')], data.days.map((day, index) => [dayLabel(day), connectedText(item.daily[index]), t('usage.82', { value: item.dailyConnections[index] })]), t('common.40'))
    : dayBars({ label: t('usage.83'), days, format: connectedText, nice: niceDuration });
  const open = people ? (ctx.userByName(item.key) ? ctx.link(t('common.141'), 'user', item.key, { class: 'button secondary' }) : null) : ctx.desktopById(item.key) ? ctx.link(t('common.136'), 'desktop', item.key, { class: 'button secondary' }) : null;
  // The page under the window cannot be reached while it is open: the window says its period and has the 그래프 / 표 choice of its own.
  const [from, to] = span(ctx);
  const view = trend ? el('div', { class: 'segmented', role: 'group', 'aria-label': t('common.129') }, [['chart', t('usage.74')], ['table', t('usage.75')]].map(([key, text]) => { const on = (key === 'table') === ui.table;
    return button(text, () => { ui.table = key === 'table'; ctx.render({ refocus: { key: 'usage-view-' + key } }); }, 'segment' + (on ? ' selected' : ''), null, { 'aria-pressed': String(on), 'data-focus': 'usage-view-' + key }); })) : null;
  return detailWindow(ctx, { key: `usage:${kind}:${item.key}`, kind: people ? t('usage.59') : t('usage.60'), title: item.name, sub: from === to ? dayLabel(from) : `${dayLabel(from)} ~ ${dayLabel(to)}`,
    className: 'usage-detail', size: 'wide', focusKey: `usage-${kind}-${item.key}`,
    close: () => { ui[people ? 'user' : 'desktop'] = null; apply(ctx, route, `usage-${kind}-${item.key}`); }, actions: [open].filter(Boolean),
    tabs: [{ key: 'detail', label: item.name, nodes: [numbers, view ? el('div', { class: 'usage-view' }, view) : null, trend,
      !people && item.users.length ? el('div', { class: 'usage-shares' }, el('h3', {}, t('usage.84')), rankBars({ label: t('usage.85', { name: item.name }), rows: item.users.map(user => ({ key: user.account, name: user.account ?? t('common.71'), value: user.connectedMs, note: t('usage.61', { connections: user.connections }) })), format: connectedText })) : null,
      el('h3', {}, people ? t('usage.86') : t('usage.87')),
      list.state ?? [eventTable(ctx, list.events, { people: !people, desktops: people, dated: data.days.length > 1, emptyTitle: t('common.40'), emptyText: t('usage.88') }), moreButton(ctx, route, list)]] }] });
}

// 로그인·연결 내역: one day's sign-ins and connections, of everyone or one person, with a search.
function history(ctx, route) {
  const ui = usageUi(ctx), end = today(ctx), day = ui.day ?? end, list = eventPages(ctx, eventNames(ctx, 'history').filter(name => ui.who === 'all' || name.includes('/user/')));
  const go = (target, key) => { ui.day = target >= end ? null : target; ui.pages = 1; ui.who = 'all'; apply(ctx, route, key); };
  const stepper = el('div', { class: 'usage-stepper', role: 'group', 'aria-label': t('usage.81') },
    button(icon('back'), () => go(shiftDay(day, -1), 'usage-prev'), 'icon-button', null, { 'aria-label': t('usage.89'), title: t('usage.89'), 'data-focus': 'usage-prev', disabled: dayNumber(end) - dayNumber(day) >= 730 }),
    el('strong', { class: 'usage-day', 'aria-live': 'polite' }, day === end ? t('usage.90', { day, weekday: weekday(day) }) : dayLabel(day)),
    button(icon('arrow'), () => go(shiftDay(day, 1), 'usage-next'), 'icon-button', null, { 'aria-label': t('usage.91'), title: t('usage.91'), 'data-focus': 'usage-next', disabled: day >= end }),
    el('input', { type: 'date', class: 'usage-day-input', value: day, min: shiftDay(end, -730), max: end, 'aria-label': t('usage.92'), 'data-focus': 'usage-day', onchange: event => { if (dayPattern.test(event.target.value)) go(event.target.value, 'usage-day'); } }),
    day === end ? null : button(t('usage.53'), () => go(end, 'usage-today'), 'quiet small', null, { 'data-focus': 'usage-today' }));
  // The whole day's counts per person come from the unfiltered first page.
  const all = ctx.value(eventsName(day, day, null, null, 1)), counts = all?.counts ?? list.counts, accounts = all?.accounts ?? list.accounts ?? [];
  const people = accounts.filter(entry => entry.account), known = people.some(entry => entry.account === ui.who);
  const who = el('label', {}, t('common.7'), select(t('common.7'), [['all', t('usage.93', { value: counts ? counts.logins + counts.connections : 0 })], ...people.map(entry => [entry.account, t('usage.94', { account: entry.account, value: entry.logins + entry.connections })]),
    ...(ui.who !== 'all' && !known ? [[ui.who, ui.who]] : [])], ui.who, value => { ui.who = value; ui.pages = 1; apply(ctx, route, 'usage-who'); }, { attrs: { 'data-focus': 'usage-who' } }));
  const q = ui.query.trim().toLowerCase();
  const shown = list.state ? [] : list.events.filter(event => !q || [event.account, event.address, event.desktopName, event.desktopId].filter(Boolean).join(' ').toLowerCase().includes(q));
  const body = list.state ?? [
    el('div', { class: 'result-bar' }, el('span', {}, el('strong', {}, t('usage.95', { shownCount: shown.length })), counts && ui.who === 'all' && !q ? t('usage.96', { logins: counts.logins, connections: counts.connections }) : null), list.more ? el('small', { class: 'muted' }, t('usage.97')) : null),
    eventTable(ctx, shown, { emptyTitle: list.events.length ? t('common.130') : t('common.40'), emptyText: list.events.length ? t('common.132') : t('usage.98') }), moreButton(ctx, route, list)];
  return [el('div', { class: 'query-box usage-history-bar', role: 'search', 'aria-label': t('usage.99') }, el('span', { class: 'query-name' }, t('usage.81')), stepper, who,
      el('label', { class: 'is-search' }, t('common.89'), ctx.searchInput({ key: 'usage-search', value: ui.query, placeholder: t('usage.100'), label: t('usage.101'), onChange: value => { ui.query = value; } }))),
    panel({ className: 'usage-history-panel' }, el('div', { class: 'usage-history-list' }, body))];
}

export function render(ctx, route) {
  const tab = tabOf(route), live = ctx.value('usageLive'), data = ctx.state.data;
  const registered = registeredHostIds(ctx), count = (value, unit) => live ? integer(value) + unit : '—';
  const strip = el('dl', { class: 'usage-strip', 'aria-label': t('usage.102') }, [[t('usage.36'), count(live?.people, t('common.105'))], [t('common.69'), count(live?.connections.length, t('usage.103'))],
    [t('usage.104'), count(live?.today.logins, t('usage.105'))], [t('usage.106'), count(live?.today.connections, t('usage.105'))],
    [t('usage.107'), live ? t('usage.108', { integer: integer(live.totals.logins) }) : '—'], [t('usage.109'), live ? t('usage.108', { integer: integer(live.totals.connections) }) : '—'], [t('common.7'), t('usage.63', { usersCount: data.users.length })],
    [t('common.5'), registered ? t('usage.110', { filter: data.desktops.filter(d => registered.has(d.id)).length }) : '—']].map(([label, value]) => el('div', {}, el('dt', {}, label), el('dd', {}, value))));
  const tabs = [['now', t('common.65')], ['users', t('usage.111')], ['desktops', t('usage.112')], ['history', t('usage.113')]];
  const pick = key => { usageUi(ctx).pages = 1; ctx.navigate('monitoring', key === usageTabs[0] ? null : key); ctx.render({ refocus: { key: 'usage-tab-' + key } }); };
  const bar = el('div', { class: 'log-tabs', role: 'tablist', 'aria-label': t('usage.114') }, tabs.map(([key, title], index) =>
    button(title, () => pick(key), 'log-tab' + (tab === key ? ' selected' : ''), null, { id: 'usage-tab-' + key, role: 'tab', 'aria-selected': String(tab === key), 'aria-controls': 'usage-panel', tabindex: tab === key ? '0' : '-1', 'data-focus': 'usage-tab-' + key,
      onkeydown: event => { const delta = event.key === 'ArrowRight' ? 1 : event.key === 'ArrowLeft' ? -1 : null; if (delta !== null || ['Home', 'End'].includes(event.key)) { event.preventDefault(); pick(tabs[event.key === 'Home' ? 0 : event.key === 'End' ? tabs.length - 1 : (index + delta + tabs.length) % tabs.length][0]); } } })));
  const refresh = button(t('common.42'), async () => { await ctx.refresh(); for (const name of usageNeeds(ctx, route)) void ctx.load(name, { force: true }); }, 'secondary', 'refresh');
  const zone = live?.timeZone ? t('usage.115', { timeZone: live.timeZone }) : t('usage.116');
  const before = live?.recordingSince && Number.isFinite(Date.parse(live.recordingSince)) ? t('usage.117', { date: date(live.recordingSince) }) : '';
  return [ctx.pageHeader({ title: t('common.3'), actions: [refresh] }), strip, bar,
    el('div', { id: 'usage-panel', class: 'log-panel usage-panel', role: 'tabpanel', 'aria-labelledby': 'usage-tab-' + tab }, { now, users: () => ranking(ctx, route, 'users'), desktops: () => ranking(ctx, route, 'desktops'), history: () => history(ctx, route) }[tab](ctx)),
    el('p', { class: 'page-note usage-note' }, icon('info'), el('span', {}, t('usage.119'), el('br'), t('usage.120', { zone, before })))];
}
