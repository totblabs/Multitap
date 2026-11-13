// 공지: a short notice to the people who are signed in or connected. One page:
// write it (who gets it, what kind, a title and a few lines), see beside it who
// can receive it right now, and below the notices already sent with how many
// apps received each and how many people saw it.
// Also 먼저 알리기 (announceFirst): the optional step in the confirmations of
// the operations that end connections. It sends a prepared notice, counts down
// and then lets the operation run exactly as it would have.
// Data: /notices (server/src/notices.mjs). The answer has no clock in it, so
// the page is redrawn only when the audience or the history really changed.
// A notice is text: it is put on the page as text nodes and nothing in it is
// linked, parsed or run.
import { el, icon, button, stateDot, table, panel, facts, emptyState, loadingState, errorState, segmented, select, date, hhmm, timeTag, disable, detailWindow } from './dom.js';
import { t } from './i18n.js';

const platforms = { windows: 'Windows', android: 'Android', web: t('common.133') };
const kindNames = { general: t('notices.1'), maintenance: t('notices.2') };
const targetNames = { all: t('notices.3'), user: t('notices.4'), desktop: t('notices.5') };
const fallbackLimits = { title: 60, body: 500, bodyLines: 12, generalHours: 24, maintenanceDays: 30 };
const length = text => [...text].length;
const pad = value => String(value).padStart(2, '0');
// A local date and time as a datetime-local field writes it.
const fieldTime = ms => { const d = new Date(ms); return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`; };
const nextHour = (now = Date.now()) => { const d = new Date(now + 3600000); d.setMinutes(0, 0, 0); return fieldTime(d.getTime()); };
// "오늘 18:00", "내일 09:30", otherwise the date.
function whenText(value, now = Date.now()) {
  const time = Date.parse(value); if (!Number.isFinite(time)) return '—';
  const day = ms => { const d = new Date(ms); return new Date(d.getFullYear(), d.getMonth(), d.getDate()).getTime(); };
  const days = Math.round((day(time) - day(now)) / 86400000);
  return days === 0 ? t('notices.6', { hhmm: hhmm(time) }) : days === 1 ? t('notices.7', { hhmm: hhmm(time) }) : date(time);
}

// 공지 쓰기 opens: a 점검 시각 that has passed meanwhile (the form is kept for the whole sign-in) starts again at the
// next full hour. Only on opening, never while the time is being typed.
function freshWhen(ui) {
  if (Date.parse(ui.when) > Date.now()) return;
  ui.when = nextHour();
  if (ui.nodes?.whenInput) ui.nodes.whenInput.value = ui.when;
}
// A notice being written when the last sign-in ended comes back (app.js keepDrafts): its text fields only.
const keptNotice = ctx => { const kept = ctx.takeDraft?.('notices'); return kept ? Object.fromEntries(Object.entries(kept).filter(([key, value]) => ['target', 'user', 'desktop', 'kind', 'title', 'body', 'when'].includes(key) ? typeof value === 'string' : key === 'composing' && typeof value === 'boolean')) : {}; };
const noticeUi = ctx => ctx.state.ui.notices ??= { target: 'all', user: '', desktop: '', kind: 'general', title: '', body: '', when: '', sending: false, error: '', nodes: null, ...keptNotice(ctx) };
const audienceOf = ctx => ctx.value('notices')?.audience ?? null;
const limitsOf = ctx => ({ ...fallbackLimits, ...(ctx.value('notices')?.limits ?? {}) });
// Opens 공지, optionally already addressed to one person or one desktop
// (사용자 and 데스크톱 offer it as 공지 보내기).
export function openNotices(ctx, patch = {}) {
  const ui = noticeUi(ctx);
  Object.assign(ui, patch, { error: '', composing: true, shown: null });
  ui.nodes = null; freshWhen(ui);
  ctx.navigate('notices');
}
// The desktops somebody is connected to now: only those can be addressed.
function busyDesktops(ctx) {
  const map = new Map();
  for (const row of audienceOf(ctx)?.connections ?? []) {
    const entry = map.get(row.desktopId) ?? { id: row.desktopId, name: row.desktopName, accounts: new Set() };
    if (row.account) entry.accounts.add(row.account);
    map.set(row.desktopId, entry);
  }
  return [...map.values()].sort((a, b) => String(a.name).localeCompare(String(b.name), 'ko', { numeric: true }));
}
const enabledUsers = ctx => (ctx.state.data?.users ?? []).filter(user => !user.disabled).sort((a, b) => a.username.localeCompare(b.username, 'ko'));

// Who the notice being written goes to, and the one line that says so.
function recipients(ctx) {
  const ui = noticeUi(ctx), audience = audienceOf(ctx), limits = limitsOf(ctx);
  const later = ui.kind === 'maintenance' ? t('notices.8') : t('notices.9', { generalHours: limits.generalHours });
  if (ui.target === 'user') {
    const user = enabledUsers(ctx).find(entry => entry.username === ui.user);
    if (!user) return { ready: false, target: null, text: t('notices.10') };
    const online = (audience?.logins ?? []).some(row => row.account === user.username);
    return { ready: true, target: { kind: 'user', username: user.username }, label: user.username,
      text: online ? t('notices.11', { username: user.username }) : t('notices.12', { username: user.username }) };
  }
  if (ui.target === 'desktop') {
    const desktop = busyDesktops(ctx).find(entry => entry.id === ui.desktop);
    if (!desktop) return { ready: false, target: null, text: busyDesktops(ctx).length ? t('notices.13') : t('notices.14') };
    const names = [...desktop.accounts];
    return { ready: true, target: { kind: 'desktop', desktopIds: [desktop.id] }, label: desktop.name, text: t('notices.17', { name: desktop.name, value: names.length ? names.join(', ') + t('notices.15') : t('notices.16') }) };
  }
  const people = audience?.people ?? 0;
  return { ready: true, target: { kind: 'all' }, label: targetNames.all, text: people ? t('notices.18', { people, later }) : t('notices.19', { later }) };
}
// The notice as its reader sees it: kind, title, the lines of its body.
function preview(notice, { time = null } = {}) {
  return el('div', { class: 'notice-preview' + (notice.kind === 'maintenance' ? ' is-maintenance' : '') },
    el('div', { class: 'notice-preview-head' }, el('span', { class: 'notice-kind' }, kindNames[notice.kind] ?? kindNames.general), time ? el('small', {}, time) : null),
    el('strong', {}, notice.title), notice.body ? el('p', {}, notice.body) : null);
}

function validate(ctx) {
  const ui = noticeUi(ctx), limits = limitsOf(ctx), title = ui.title.replace(/\s+/g, ' ').trim(), body = ui.body.trim();
  if (!title) return { error: t('notices.20'), focus: 'notice-title' };
  if (length(title) > limits.title) return { error: t('notices.21', { title: limits.title }), focus: 'notice-title' };
  if (length(body) > limits.body || body.split('\n').length > limits.bodyLines) return { error: t('notices.22', { body: limits.body, bodyLines: limits.bodyLines }), focus: 'notice-body' };
  const to = recipients(ctx);
  if (!to.ready) return { error: to.text.split('\n')[0], focus: null };
  let scheduledAt = null;
  if (ui.kind === 'maintenance') {
    const at = Date.parse(ui.when);
    if (!Number.isFinite(at) || at <= Date.now()) return { error: t('notices.23'), focus: 'notice-when' };
    if (at > Date.now() + limits.maintenanceDays * 86400000) return { error: t('notices.24', { maintenanceDays: limits.maintenanceDays }), focus: 'notice-when' };
    scheduledAt = new Date(at).toISOString();
  }
  return { input: { kind: ui.kind, title, body, target: to.target, ...(scheduledAt ? { scheduledAt } : {}) }, to };
}
function send(ctx) {
  const ui = noticeUi(ctx), checked = validate(ctx);
  if (ui.sending) return;
  // What is wrong is said under the field it is about, which is marked and takes the focus.
  if (checked.error) { ui.error = checked.error; ui.errorField = checked.focus; sync(ctx); if (checked.focus) ui.nodes?.root.querySelector(`[data-focus="${checked.focus}"]`)?.focus?.(); return; }
  ui.error = ''; sync(ctx);
  const { input, to } = checked;
  ctx.modal(t('notices.25'), [
    { node: el('p', { class: 'confirm-who is-calm' }, icon('user'), el('span', {}, t('notices.26') + to.label)) },
    { node: preview(input, { time: input.scheduledAt ? t('notices.27', { whenText: whenText(input.scheduledAt) }) : null }) },
    { node: el('p', { class: 'modal-description' }, to.text + t('notices.28')) },
  ], async () => {
    const action = ctx.pageAction('notice-send');
    ui.sending = true; sync(ctx);
    try {
      await ctx.api('/notices', 'POST', input);
      action.check();
      ui.title = ''; ui.body = ''; ui.error = ''; ui.nodes = null; ui.composing = false;
      ctx.notice(t('notices.29'));
      void ctx.load('notices', { force: true });
    } finally { ui.sending = false; if (action.current()) sync(ctx); }
  }, { save: t('notices.30'), reload: false });
}
function withdraw(ctx, notice) {
  ctx.modal(t('notices.31'), [{ node: preview(notice) }], async () => {
    const action = ctx.pageAction('notice-withdraw');
    await ctx.api('/notices/' + notice.id + '/withdraw', 'POST', {});
    action.check();
    ctx.notice(t('notices.32'));
    void ctx.load('notices', { force: true });
  }, { save: t('notices.33'), danger: true, reload: false, safeFocus: true,
    description: t('notices.34') });
}

// ---- 공지 쓰기: the form of a window (render). It is built once and kept while the page is redrawn, so
// the text being typed (and a Korean syllable being composed) is not disturbed
// by an update of the list beside it. sync() brings its small changing parts
// up to date in place.
function buildForm(ctx) {
  const ui = noticeUi(ctx), limits = limitsOf(ctx);
  if (!ui.when) ui.when = nextHour();
  const nodes = {};
  const counter = (text, max) => `${length(text)}/${max}`;
  nodes.targets = el('div', { class: 'notice-choice' });
  nodes.targetDetail = el('div', { class: 'notice-target-detail' });
  nodes.recipients = el('p', { class: 'notice-recipients', role: 'status' });
  nodes.kinds = el('div', { class: 'notice-choice' });
  nodes.whenInput = el('input', { type: 'datetime-local', value: ui.when, 'data-focus': 'notice-when', 'aria-label': t('notices.35'), oninput: event => { ui.when = event.target.value; ui.error = ''; sync(ctx); } });
  nodes.when = el('label', { class: 'notice-when' }, t('notices.35'), nodes.whenInput, el('small', { class: 'field-hint' }, t('notices.36')));
  nodes.titleCount = el('small', { class: 'notice-count' }, counter(ui.title, limits.title));
  nodes.title = el('input', { type: 'text', value: ui.title, maxlength: limits.title, placeholder: t('notices.37'), autocomplete: 'off', 'data-focus': 'notice-title',
    oninput: event => { ui.title = event.target.value; nodes.titleCount.textContent = counter(ui.title, limits.title); if (ui.error) { ui.error = ''; sync(ctx); } } });
  nodes.bodyCount = el('small', { class: 'notice-count' }, counter(ui.body, limits.body));
  nodes.body = el('textarea', { maxlength: limits.body, rows: '5', placeholder: t('notices.38'), 'data-focus': 'notice-body',
    oninput: event => { ui.body = event.target.value; nodes.bodyCount.textContent = counter(ui.body, limits.body); if (ui.error) { ui.error = ''; sync(ctx); } } });
  nodes.body.value = ui.body;
  nodes.error = el('p', { class: 'error notice-error', role: 'alert', id: 'notice-error' });
  nodes.send = button(t('notices.30'), () => send(ctx), 'primary', 'megaphone');
  nodes.root = el('div', { class: 'notice-form' }, el('p', { class: 'field-hint' }, t('notices.39')),
    el('div', { class: 'notice-field' }, el('span', { class: 'stack-label' }, t('notices.40')), nodes.targets, nodes.targetDetail, nodes.recipients),
    el('div', { class: 'notice-field' }, el('span', { class: 'stack-label' }, t('common.62')), nodes.kinds, nodes.when),
    el('label', {}, el('span', { class: 'notice-label-row' }, t('notices.41'), nodes.titleCount), nodes.title),
    el('label', {}, el('span', { class: 'notice-label-row' }, t('notices.42'), nodes.bodyCount), nodes.body),
    nodes.error);
  ui.nodes = nodes;
  return nodes;
}
function sync(ctx) {
  const ui = noticeUi(ctx), nodes = ui.nodes;
  if (!nodes) return;
  const choose = (container, name, options, value, onChange) => {
    const key = name + ':' + value;
    if (container.dataset.key === key) return;
    container.dataset.key = key;
    container.replaceChildren(segmented(name, options, value, onChange));
  };
  const refocus = (container, key) => container.querySelector(`[data-segment="${key}"]`)?.focus?.();
  choose(nodes.targets, t('notices.40'), Object.entries(targetNames), ui.target, key => { ui.target = key; ui.error = ''; sync(ctx); refocus(nodes.targets, key); });
  choose(nodes.kinds, t('common.62'), Object.entries(kindNames), ui.kind, key => { ui.kind = key; ui.error = ''; sync(ctx); refocus(nodes.kinds, key); });
  // The person or desktop choice: rebuilt only when its options changed.
  const online = new Set((audienceOf(ctx)?.logins ?? []).map(row => row.account));
  const users = enabledUsers(ctx), desktops = busyDesktops(ctx);
  const detailKey = ui.target === 'user' ? 'user:' + ui.user + '|' + users.map(user => user.username + (online.has(user.username) ? '*' : '')).join(',')
    : ui.target === 'desktop' ? 'desktop:' + ui.desktop + '|' + desktops.map(desktop => desktop.id + desktop.name + desktop.accounts.size).join(',') : 'all';
  if (nodes.targetDetail.dataset.key !== detailKey) {
    nodes.targetDetail.dataset.key = detailKey;
    nodes.targetDetail.replaceChildren(...(ui.target === 'user' ? [select(t('notices.43'), [['', t('notices.44')], ...users.map(user => [user.username, user.username + (online.has(user.username) ? t('notices.45') : '')])], ui.user, value => { ui.user = value; ui.error = ''; sync(ctx); }, { className: 'filter-select notice-select', attrs: { 'data-focus': 'notice-user' } })]
      : ui.target === 'desktop' && desktops.length ? [select(t('common.5'), [['', t('notices.46')], ...desktops.map(desktop => [desktop.id, t('notices.47', { name: desktop.name, size: desktop.accounts.size })])], ui.desktop, value => { ui.desktop = value; ui.error = ''; sync(ctx); }, { className: 'filter-select notice-select', attrs: { 'data-focus': 'notice-desktop' } })] : []));
  }
  const to = recipients(ctx);
  nodes.recipients.textContent = to.text;
  nodes.recipients.classList.toggle('is-waiting', !to.ready);
  nodes.when.hidden = ui.kind !== 'maintenance';
  nodes.whenInput.setAttribute('min', fieldTime(Date.now()));
  nodes.error.textContent = ui.error;
  const wrong = ui.error ? { 'notice-title': nodes.title, 'notice-body': nodes.body, 'notice-when': nodes.whenInput }[ui.errorField] ?? null : null;
  for (const field of [nodes.title, nodes.body, nodes.whenInput]) {
    if (field === wrong) { field.setAttribute('aria-invalid', 'true'); field.setAttribute('aria-describedby', 'notice-error'); }
    else { field.removeAttribute('aria-invalid'); field.removeAttribute('aria-describedby'); }
  }
  if (wrong) { if (nodes.error.previousSibling !== wrong) wrong.after(nodes.error); } else if (nodes.error.parentNode !== nodes.root) nodes.root.append(nodes.error);
  nodes.send.textContent = ''; nodes.send.append(icon('megaphone'), ui.sending ? t('notices.48') : t('notices.30'));
  disable(nodes.send, ui.sending ? t('notices.49') : !ctx.value('notices') ? t('notices.50') : null);
}

// 지금 받을 수 있는 사람: every sign-in with its app and what it is connected to.
function audiencePanel(ctx) {
  const entry = ctx.resource('notices'), audience = entry?.value?.audience, ui = noticeUi(ctx);
  if (!audience) return panel({ title: t('notices.51'), className: 'notice-audience' }, entry?.error ? errorState(t('notices.52'), entry.error, () => ctx.load('notices', { force: true })) : loadingState(t('notices.53')));
  const rows = audience.logins.map(login => {
    const connected = audience.connections.filter(row => row.account === login.account);
    const names = [...new Set(connected.map(row => row.desktopName))];
    return el('li', { class: 'notice-person' },
      el('span', { class: 'notice-person-text' }, el('strong', {}, login.account ?? t('common.121')),
        el('small', {}, [[platforms[login.platform] ?? t('notices.54'), login.version ? login.version.replace(/\+\d+$/, '') : null].filter(Boolean).join(' '), login.since ? t('notices.55', { hhmm: hhmm(login.since) }) : null].filter(Boolean).join(' · '))),
      login.connections && names.length ? el('span', { class: 'notice-where', title: names.join(', ') }, names.length === 1 ? names[0] : t('notices.56', { value: names[0], value2: names.length - 1 })) : el('span', { class: 'notice-where muted' }, t('notices.57')),
      button(t('notices.58'), () => { ui.target = 'user'; ui.user = login.account; ui.error = ''; ui.composing = true; ui.shown = null; freshWhen(ui); ctx.render(); ui.nodes?.title.focus?.(); }, 'quiet small', null, { 'aria-label': t('notices.59', { account: login.account }) }));
  });
  return panel({ title: t('notices.51'), count: audience.people, className: 'notice-audience' },
    rows.length ? el('ul', { class: 'notice-people', 'data-scroll-key': 'notice-people' }, rows)
      : emptyState(t('common.134'), t('notices.61'), { compact: true, iconName: 'user' }));
}

// 보낸 공지: newest first.
function historyPanel(ctx) {
  const entry = ctx.resource('notices'), list = entry?.value?.notices;
  if (!list) return panel({ title: t('notices.62'), className: 'notice-history' }, entry?.error ? errorState(t('notices.52'), entry.error, () => ctx.load('notices', { force: true })) : loadingState(t('notices.63')));
  const target = notice => notice.target.kind === 'all' ? targetNames.all : notice.target.label;
  const rows = list.map(notice => [
    timeTag(notice.sentAt, date(notice.sentAt)),
    notice.by,
    el('span', { class: 'notice-to' }, target(notice), notice.source === 'operation' ? el('small', {}, t('notices.64')) : null),
    el('div', { class: 'notice-cell' }, el('span', { class: 'notice-cell-head' }, el('span', { class: 'notice-kind' }, kindNames[notice.kind] ?? kindNames.general),
      notice.scheduledAt ? el('small', {}, t('notices.27', { whenText: whenText(notice.scheduledAt) })) : null), el('strong', {}, notice.title), notice.body ? el('p', {}, notice.body) : null),
    el('span', { class: 'notice-number', title: t('notices.65') }, t('notices.66', { apps: notice.apps })),
    el('span', { class: 'notice-number', title: t('notices.67') }, t('notices.68', { seen: notice.seen }), notice.people ? el('small', {}, t('notices.69', { people: notice.people })) : null),
    notice.active ? el('span', { class: 'notice-state' }, stateDot(t('notices.70'), 'green'), el('small', {}, t('notices.71', { whenText: whenText(notice.expiresAt) })), button(t('notices.33'), () => withdraw(ctx, notice), 'quiet small', null, { 'aria-label': t('notices.72', { title: notice.title }) }))
      : el('span', { class: 'notice-state' }, stateDot(notice.withdrawnAt ? t('notices.73') : t('notices.74')), el('small', {}, date(notice.withdrawnAt ?? notice.expiresAt))),
    button(t('common.91'), () => { noticeUi(ctx).shown = notice.id; noticeUi(ctx).composing = false; ctx.render(); }, 'secondary small', null, { 'aria-label': t('common.131', { title: notice.title }), 'data-focus': 'notice-open-' + notice.id }),
  ]);
  const retention = limitsOf(ctx).retentionDays;
  return panel({ title: t('notices.62'), count: list.length, className: 'notice-history', description: retention ? t('notices.75', { retention }).trim() : null },
    table([t('notices.77'), t('notices.78'), t('notices.40'), t('common.4'), t('notices.79'), t('notices.80'), t('common.61'), t('common.85')], rows, t('notices.81'), t('notices.82'), { className: 'notice-table' }));
}
// One sent notice in a window: as its reader saw it, then who sent it to whom and how many received and saw it.
function noticeWindow(ctx, notice) {
  const ui = noticeUi(ctx), close = () => { ui.shown = null; ctx.render({ refocus: { key: 'notice-open-' + notice.id } }); };
  return detailWindow(ctx, { key: 'notice:' + notice.id, kind: t('notices.62'), title: notice.title, size: 'narrow', className: 'notice-window', close,
    badges: notice.active ? stateDot(t('notices.70'), 'green') : stateDot(notice.withdrawnAt ? t('notices.73') : t('notices.74')),
    tabs: [{ key: 'notice', label: t('notices.62'), nodes: [preview(notice, { time: notice.scheduledAt ? t('notices.27', { whenText: whenText(notice.scheduledAt) }) : null }), facts([
      [t('notices.77'), date(notice.sentAt)], [t('notices.78'), notice.by],
      [t('notices.40'), notice.target.kind === 'all' ? targetNames.all : notice.target.label, notice.source === 'operation' ? t('notices.64') : null],
      [t('notices.79'), el('span', { title: t('notices.65') }, t('notices.66', { apps: notice.apps }))],
      [t('notices.80'), el('span', { title: t('notices.67') }, notice.people ? t('notices.seenOf', { seen: notice.seen, people: notice.people }) : t('notices.68', { seen: notice.seen }))],
      [t('common.61'), `${notice.active ? t('notices.70') : notice.withdrawnAt ? t('notices.73') : t('notices.74')} · ${notice.active ? t('notices.71', { whenText: whenText(notice.expiresAt) }) : date(notice.withdrawnAt ?? notice.expiresAt)}`]], 'form-table')] }],
    actions: notice.active ? [button(t('notices.33'), () => withdraw(ctx, notice), 'danger')] : [] });
}

export function render(ctx) {
  const ui = noticeUi(ctx);
  // One form for one sign-in and one set of limits; a new one after a send.
  const nodes = ui.nodes ?? buildForm(ctx);
  sync(ctx);
  // 공지 쓰기 is a window over the page: the form in it, 보내기 in its bar.
  const write = () => { ui.composing = true; ui.shown = null; freshWhen(ui); ctx.render(); };
  const shown = ui.shown ? (ctx.value('notices')?.notices ?? []).find(notice => notice.id === ui.shown) ?? null : null;
  const compose = ui.composing ? detailWindow(ctx, { key: 'notice-compose', kind: t('common.4'), title: t('notices.84'), size: 'narrow', className: 'notice-compose', focusKey: 'notice-write',
    close: () => { ui.composing = false; ctx.render({ refocus: { key: 'notice-write' } }); }, tabs: [{ key: 'form', label: t('notices.84'), nodes: nodes.root }], main: nodes.send, dirty: () => !!(ui.title.trim() || ui.body.trim()) }) : null;
  return [ctx.pageHeader({ title: t('common.4'), actions: [button(t('notices.84'), write, 'primary', 'megaphone', { 'data-focus': 'notice-write' })] }),
    historyPanel(ctx), audiencePanel(ctx), compose ?? (shown ? noticeWindow(ctx, shown) : null)];
}

// ---- 먼저 알리기 ------------------------------------------------------------
export const announceWaits = [[0, t('notices.86')], [60, t('notices.87')], [300, t('notices.88')]];
const waitWords = { 0: t('notices.89'), 60: t('notices.90'), 300: t('notices.91') };
// "지금 바로" still leaves the apps a moment to read the notice before the connection goes.
export const announceMinimumMs = 3000;
const clockText = ms => { const seconds = Math.max(0, Math.ceil(ms / 1000)); return `${Math.floor(seconds / 60)}:${pad(seconds % 60)}`; };
// Everyone signed in, for the operations that end every connection; null when nobody is.
export function everyoneAudience(ctx) {
  const audience = audienceOf(ctx);
  return audience && audience.apps > 0 ? { target: { kind: 'all' }, people: audience.people } : null;
}
// The prepared texts. `when` is 곧, 1분 뒤 or 5분 뒤.
const short = name => { const text = String(name ?? '').trim(); return length(text) > 28 ? [...text].slice(0, 27).join('') + '…' : text; };
export const announcements = {
  serverUpdate: { title: t('notices.92'), body: when => t('notices.93', { when }), then: t('notices.92') },
  serverRestart: { title: t('notices.serverRestart'), body: when => t('notices.93', { when }), then: t('notices.serverRestart') },
  serverAccess: { title: t('notices.94'), body: when => t('notices.95', { when }), then: t('notices.96') },
  restore: { title: t('notices.97'), body: when => t('notices.98', { when }), then: t('notices.99') },
  hostUpdate: { title: t('notices.100'), body: when => t('notices.101', { when }), then: t('notices.102') },
  reboot: name => ({ title: t('notices.103', { short: short(name) }), body: when => t('notices.104', { when }), then: t('notices.105') }),
  shutdown: name => ({ title: t('notices.106', { short: short(name) }), body: when => t('notices.107', { when }), then: t('notices.108') }),
};
// The optional step of a confirmation that ends connections.
//   audience: { target, people } (who is told; people may be null when only
//             "somebody" is known) or null/absent: nobody is connected, so
//             the option is not offered and null is returned;
//   text:     one of `announcements`.
// Returns { node, wrap }: `node` goes into the dialog, and wrap(action) is the
// dialog's action. Unticked, wrap(action) is action itself. Ticked, it sends
// the notice, shows the countdown with 취소 and then runs action; 취소 ends it
// before the operation, withdraws the notice and leaves the dialog open.
export function announceFirst(ctx, { audience, text, now = () => Date.now(), tickMs = 250 } = {}) {
  if (!audience?.target || audience.people === 0 || !text) return null;
  const people = Number.isSafeInteger(audience.people) && audience.people > 0 ? t('notices.109', { people: audience.people }) : t('notices.16');
  let wait = 60, running = null;
  const box = el('input', { type: 'checkbox', name: 'announceFirst', 'data-announce': 'toggle' });
  const waits = el('div', { class: 'announce-waits' });
  const shown = el('div', { class: 'notice-preview is-maintenance announce-preview' });
  const options = el('div', { class: 'announce-options', hidden: true }, el('span', { class: 'stack-label' }, t('notices.110')), waits, shown,
    el('small', { class: 'field-hint' }, t('notices.111', { people })));
  const status = el('strong', {}), remaining = el('span', { class: 'announce-remaining' });
  const cancel = button(t('common.135'), () => running?.cancel(), 'secondary', null, { 'data-announce': 'cancel' });
  const countdown = el('div', { class: 'announce-countdown', role: 'status', hidden: true }, el('span', { class: 'loading-dot', 'aria-hidden': 'true' }), el('div', {}, status, remaining), cancel);
  // 취소 is what the administrator chose, not something that failed: it is said here, in plain words, and the
  // dialog's red error line stays empty (the dialog stays open because its action still ends without running).
  const cancelled = el('p', { class: 'announce-note', role: 'status', hidden: true });
  const draw = () => {
    waits.replaceChildren(segmented(t('notices.110'), announceWaits.map(([seconds, label]) => [String(seconds), label]), String(wait), key => { wait = Number(key); draw(); waits.querySelector(`[data-segment="${key}"]`)?.focus?.(); }, { disabled: !!running }));
    shown.replaceChildren(el('div', { class: 'notice-preview-head' }, el('span', { class: 'notice-kind' }, kindNames.maintenance), el('small', {}, t('notices.112'))), el('strong', {}, text.title), el('p', {}, text.body(waitWords[wait])));
    options.hidden = !box.checked || !!running; box.disabled = !!running;
  };
  box.addEventListener('change', draw);
  const node = el('div', { class: 'announce-first' }, el('label', { class: 'announce-toggle' }, box, el('span', {}, t('notices.64')), el('small', {}, t('notices.113'))), options, countdown, cancelled);
  draw();
  const wrap = action => async input => {
    cancelled.hidden = true;
    if (!box.checked) return action(input);
    const waitSeconds = wait;
    let sent = null;
    try { sent = (await ctx.api('/notices', 'POST', { title: text.title, body: text.body(waitWords[waitSeconds]), target: audience.target, operation: { waitSeconds } })).notice; }
    catch (error) {
      // Everyone left in the meantime: there is nobody to wait for.
      if (error.code !== 'notice_no_recipients') throw error;
    }
    if (sent) {
      const until = now() + Math.max(announceMinimumMs, waitSeconds * 1000);
      await new Promise((resolve, reject) => {
        const dialog = node.closest?.('dialog') ?? null;
        const finish = (settle, value) => { clearInterval(timer); running = null; countdown.hidden = true; dialog?.classList.remove('is-announcing'); draw(); settle(value); };
        const tick = () => {
          // The dialog was closed or replaced (a sign-out, another dialog): nothing runs.
          if (!node.isConnected) return finish(reject, Object.assign(new Error(t('notices.114')), { code: 'announce_cancelled' }));
          const left = until - now();
          if (left <= 0) return finish(resolve);
          remaining.textContent = t('notices.115', { clockText: clockText(left), then: text.then });
        };
        const timer = setInterval(tick, tickMs); timer.unref?.();
        running = { cancel: () => {
          cancelled.textContent = t('notices.116'); cancelled.hidden = false;
          finish(reject, Object.assign(new Error(''), { code: 'announce_cancelled' }));
          void ctx.api('/notices/' + sent.id + '/withdraw', 'POST', {}).catch(() => {});
        } };
        status.textContent = t('notices.117', { addressed: sent.addressed });
        countdown.hidden = false; dialog?.classList.add('is-announcing'); draw(); tick();
      });
    }
    return action(input);
  };
  return { node, wrap };
}
