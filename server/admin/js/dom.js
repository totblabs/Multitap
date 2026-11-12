// DOM helpers shared by every console page. Text always enters the page as
// text nodes or attribute values, never as markup, and nothing here keeps
// module state, so each console instance renders independently.
import { createIcon } from '../icons.js';
import { t, lang, locale } from './i18n.js';

export const icon = createIcon;
const svgNamespace = 'http://www.w3.org/2000/svg';

function assign(node, attrs) {
  for (const [key, value] of Object.entries(attrs)) {
    if (value === null || value === undefined || value === false) continue;
    // on* is an event and takes a function; anything else is a mistake, never an inline handler attribute.
    if (key.startsWith('on')) { if (typeof value !== 'function') throw new TypeError('event handler is not a function: ' + key); node.addEventListener(key.slice(2), value); }
    else if (key === 'class') node.setAttribute('class', value);
    else if (key === 'text') node.textContent = value;
    else {
      node.setAttribute(key, value === true ? '' : value);
      // Form state is a property too, so a script reading .checked agrees with the markup.
      if (key === 'checked') node.checked = true;
    }
  }
}
function append(node, children) {
  for (const child of children.flat(Infinity)) if (child !== null && child !== undefined && child !== false) node.append(child instanceof globalThis.Node ? child : globalThis.document.createTextNode(String(child)));
}
export function el(tag, attrs = {}, ...children) {
  const node = globalThis.document.createElement(tag);
  assign(node, attrs); append(node, children);
  return node;
}
export function svg(tag, attrs = {}, ...children) {
  const node = globalThis.document.createElementNS(svgNamespace, tag);
  assign(node, attrs); append(node, children);
  return node;
}
export const $ = selector => globalThis.document.querySelector(selector);

// HTTP pages may lack Clipboard API. Select only the already visible text;
// never use execCommand, a hidden editable field, or an untrusted HTML string.
export function selectVisibleText(node) {
  try {
    const doc = node?.ownerDocument ?? globalThis.document;
    const selection = globalThis.getSelection?.() ?? doc?.getSelection?.();
    const range = doc?.createRange?.();
    if (!node || !selection || !range?.selectNodeContents) return false;
    range.selectNodeContents(node); selection.removeAllRanges(); selection.addRange(range);
    return true;
  } catch { return false; }
}


export function button(text, action, cls = 'secondary', iconName = null, attrs = {}) {
  return el('button', { type: 'button', class: cls, onclick: action, ...attrs }, iconName ? icon(iconName) : null, text);
}
// A disabled control says why: `reason` (falsy enables it) becomes its tooltip.
export function disable(node, reason) {
  node.disabled = !!reason;
  if (reason) node.title = reason; else node.removeAttribute?.('title');
  return node;
}
// A state, in a list, a form table or a window's title bar: one small dot in the state's colour and its word, the
// rest of the text plain (tone 'green', 'blue', 'red', 'amber', 'muted'; none is grey). Only a problem colours the
// word too (red 접속 불가, grey 사용 중지; pages.css). There are no coloured badges.
export const stateDot = (text, tone = '', attrs = {}) => el('span', { class: 'state-dot' + (tone ? ' is-' + tone : ''), ...attrs }, el('i', { 'aria-hidden': 'true' }), text);
export const count = value => el('span', { class: 'count' }, String(value));
// The numbers above a list: tally([['총', 32, '대'], ['접속 불가', 5, '', 'bad']]) reads "총 32대 · 접속 불가 5".
export const tally = (parts, ...more) => el('p', { class: 'list-count' }, el('span', {}, parts.filter(Boolean).flatMap(([before, value, after = '', tone = ''], index) =>
  [index ? ' · ' : null, el('span', { class: tone ? 'is-' + tone : null }, before ? before + ' ' : null, el('b', {}, String(value)), after)])), more);
// Above a board: how many there are on the left (tally), the page's actions on the right (the main one last).
export const listHead = (count, actions = []) => el('div', { class: 'list-head' }, count, actions.filter(Boolean).length ? el('div', { class: 'list-actions' }, actions) : null);
// 조회 조건: the conditions of a list in one box, each a named field (queryField). The list changes when 조회 is
// pressed (or Enter in a text field, queryText), and 초기화 clears every condition and shows the whole list.
export function queryBox(label, fields, onApply, onReset) {
  return el('div', { class: 'query-box', role: 'search', 'aria-label': label }, fields,
    el('div', { class: 'query-actions' }, button(t('dom.1'), () => onApply(), 'primary', null, { 'data-focus': 'query-apply' }), button(t('common.99'), () => onReset(), 'secondary', null, { 'data-focus': 'query-reset' })));
}
export const queryField = (name, control, cls = '') => el('label', { class: cls || null }, name, control);
// The conditions of a 조회 조건 box that were changed and not applied yet. `keys` name the fields of `ui` the
// list reads. values() is what the box shows, set() changes it, apply() makes the list read it, and clear() puts
// `defaults` back. Conditions changed from elsewhere (a link that opens the list filtered) drop what was pending.
export function conditions(ui, keys, defaults = {}) {
  const of = () => keys.map(key => String(ui[key])).join('\n');
  const values = () => ui.draft?.of === of() ? ui.draft : { ...Object.fromEntries(keys.map(key => [key, ui[key]])), of: of() };
  return { values, set: patch => { ui.draft = { ...values(), ...patch }; },
    apply: () => { const next = values(); for (const key of keys) ui[key] = next[key]; ui.draft = null; }, clear: () => { Object.assign(ui, defaults); ui.draft = null; } };
}
// A text condition keeps what is typed (also through a re-render, by its data-focus key). Enter applies it once
// a Korean composition has ended, and the focus and the caret stay in the field (onApply receives where).
export const queryText = ({ key, value = '', placeholder, onInput, onApply }) => el('input', { type: 'search', value, placeholder, 'data-focus': key, autocomplete: 'off', spellcheck: 'false',
  oninput: event => onInput(event.target.value), onkeydown: event => {
    if (event.key !== 'Enter' || event.isComposing) return;
    const node = event.target, end = node.value.length;
    event.preventDefault?.(); onInput(node.value); onApply({ key, range: [node.selectionStart ?? end, node.selectionEnd ?? end, node.selectionDirection] });
  } });
// A board: the list of a page as a table with a line around every cell. columns: [{ label, cls }] (cls is the
// cell's class and, on the heading, its width; head, when given, is what the heading shows instead of label); rows: { cells, attrs }, { group: [nodes], cls } for a heading
// row across the table, or { detail: [nodes] } for the detail of the row above, opened under it. A phone shows the cells marked is-name and is-state only (console.css).
export function board(columns, rows, { className = '', empty = null } = {}) {
  const span = String(columns.length);
  const body = rows.map(row => row.group ? el('tr', { class: 'board-group' + (row.cls ? ' ' + row.cls : '') }, el('th', { colspan: span }, row.group))
    : row.detail ? el('tr', { class: 'board-detail' }, el('td', { colspan: span }, row.detail))
    : el('tr', row.attrs ?? {}, row.cells.map((cell, index) => el('td', { class: columns[index].cls ?? null, 'data-label': columns[index].label }, cell))));
  return el('div', { class: 'table-scroll' }, el('table', { class: 'board' + (className ? ' ' + className : '') },
    el('thead', {}, el('tr', {}, columns.map(column => el('th', { scope: 'col', class: column.cls ?? null }, column.head ?? column.label)))),
    el('tbody', {}, body.length ? body : el('tr', { class: 'board-empty' }, el('td', { colspan: span }, empty)))));
}
export const mono = (text, attrs = {}) => el('code', { class: 'mono-chip', ...attrs }, text);

// Empty, loading and failure states share one centred layout.
export function emptyState(title, description = '', { iconName = 'inbox', tone = '', role = null, action = null, panel = false, compact = false } = {}) {
  return el(panel ? 'section' : 'div', { class: (panel ? 'panel ' : '') + 'empty' + (tone ? ' ' + tone : '') + (compact ? ' compact' : ''), role },
    iconName === 'loading' ? el('span', { class: 'loading-dot', 'aria-hidden': 'true' }) : el('span', { class: 'empty-icon' }, icon(iconName)),
    el('strong', {}, title), description ? el('p', {}, description) : null, action);
}
// Content still loading: placeholder lines in its place, and what is loading.
export const loadingState = (title = t('dom.2'), description = '') => el('div', { class: 'empty compact is-loading', role: 'status', 'aria-busy': 'true' },
  el('span', { class: 'bones', 'aria-hidden': 'true' }, el('span', { class: 'bone' }), el('span', { class: 'bone' }), el('span', { class: 'bone' })),
  el('strong', {}, title), description ? el('p', {}, description) : null);
export const errorState = (title, description, retry) => emptyState(title, description, { iconName: 'alert', tone: 'is-error', role: 'alert', compact: true, action: retry ? button(t('common.41'), retry, 'secondary', 'refresh') : null });

// Data tables keep real table semantics; every cell carries its column name
// so narrow screens can lay rows out as labelled cards (console.css).
export function table(headers, rows, emptyTitle, emptyDescription = '', { className = '', rowAttrs = () => ({}), after = () => null } = {}) {
  if (!rows.length) return emptyState(emptyTitle, emptyDescription, { compact: true });
  const body = [];
  rows.forEach((row, index) => {
    body.push(el('tr', { role: 'row', ...rowAttrs(index) }, row.map((cell, column) => el('td', { role: 'cell', 'data-label': headers[column] }, cell))));
    const extra = after(index); if (extra) body.push(extra);
  });
  return el('div', { class: 'table-scroll' }, el('table', { class: 'data-table' + (className ? ' ' + className : ''), role: 'table' },
    el('thead', {}, el('tr', { role: 'row' }, headers.map(h => el('th', { scope: 'col', role: 'columnheader' }, h)))), el('tbody', {}, body)));
}

// A panel: optional heading row with title, count, description and actions.
export function panel({ title = null, count: total = null, description = null, actions = [], className = '', id = null, level = 'h2' } = {}, ...children) {
  const head = title ? el('div', { class: 'panel-head' }, el('div', { class: 'panel-title' }, el(level, {}, title, total === null ? null : count(total)), description ? el('p', {}, description) : null), actions.length ? el('div', { class: 'actions' }, actions) : null) : null;
  return el('section', { class: 'panel' + (className ? ' ' + className : ''), id }, head, children);
}
// The window (창) of one item of a list: a charcoal title bar with what it is (`kind`), its name (`title`), one
// line under it (`sub`), its state (`badges`) and an X; tabs ({ key, label, count, nodes }; every tab's content is
// in the page and the ones not shown are hidden, so the window is the same on every tab); and a bar with the
// item's actions on the left and 닫기 on the right. A page returns it among its nodes and the console shows it in a
// modal over the page (app.js render). `close` leaves the window; `focusKey` is the data-focus of the row the
// focus then returns to. The open tab is kept in ctx.state.ui.detailTab while the window is open. A window with
// one tab shows no tab strip; `size` ('narrow', 'wide') sets its width (console.css).
export function detailWindow(ctx, { key, kind, title, sub = null, subTitle = null, badges = null, tabs, actions = [], main = null, close, focusKey = null, className = '', headClass = '', size = '', dirty = null }) {
  const shown = tabs.filter(Boolean), saved = ctx.state.ui.detailTab?.key === key ? ctx.state.ui.detailTab.tab : null;
  const current = shown.some(tab => tab.key === saved) ? saved : shown[0].key;
  const pick = tab => { ctx.state.ui.detailTab = { key, tab }; ctx.render({ refocus: { key: 'detail-tab:' + tab } }); };
  // Arrow keys move between the tabs, as in any tab strip.
  const onKey = (event, index) => {
    const next = event.key === 'ArrowRight' ? index + 1 : event.key === 'ArrowLeft' ? index - 1 : event.key === 'Home' ? 0 : event.key === 'End' ? shown.length - 1 : null;
    if (next === null) return;
    event.preventDefault?.(); pick(shown[(next + shown.length) % shown.length].key);
  };
  const single = shown.length === 1;
  main?.classList?.add('win-main');
  const frame = el('div', { class: 'win-frame' + (size ? ' is-' + size : '') + (className ? ' ' + className : ''), 'data-window': key, 'data-return-focus': focusKey },
    el('header', { class: 'win-head' + (headClass ? ' ' + headClass : '') }, el('span', { class: 'win-kind', id: 'detail-kind' }, kind), el('h2', { id: 'detail-title', tabindex: '-1' }, title),
      sub ? el('small', { title: subTitle }, sub) : null, badges,
      el('button', { type: 'button', class: 'icon-button win-close', 'aria-label': t('common.31'), onclick: () => close() }, icon('x'))),
    single ? null : el('div', { class: 'win-tabs', role: 'tablist', 'aria-label': kind }, shown.map((tab, index) => el('button', { type: 'button', role: 'tab', id: 'detail-tab-' + tab.key, 'aria-selected': String(tab.key === current), 'aria-controls': 'detail-panel-' + tab.key,
      tabindex: tab.key === current ? '0' : '-1', 'data-focus': 'detail-tab:' + tab.key, onclick: () => pick(tab.key), onkeydown: event => onKey(event, index) }, tab.label, tab.count === undefined || tab.count === null ? null : [' ', el('b', {}, String(tab.count))]))),
    el('div', { class: 'win-body', tabindex: '-1', 'data-scroll-key': `detail:${key}:${current}` }, shown.map(tab => el('section', { class: 'win-panel', role: single ? null : 'tabpanel', id: 'detail-panel-' + tab.key, 'aria-labelledby': single ? null : 'detail-tab-' + tab.key, 'data-tab': tab.key, hidden: tab.key !== current }, tab.nodes))),
    // The bar of the window: other things to do on the left (plain buttons, 삭제 first); on the right what the
    // window is for (the one dark button), then 닫기, always the light one and always last.
    el('footer', { class: 'win-foot' }, el('div', { class: 'win-actions' }, actions), el('span', { class: 'win-fill' }), main, button(t('common.31'), () => close(), 'secondary win-done')));
  // A window with something written in it does not close on a press beside it (app.js).
  if (dirty) frame.windowDirty = dirty;
  return frame;
}
// Label/value rows: a form table ('form-table' stands alone with the thick line on top, 'is-pairs' puts two on a line).
export function facts(rows, className = '') {
  return el('dl', { class: 'facts' + (className ? ' ' + className : '') }, rows.filter(Boolean).map(([label, value, hint]) => el('div', {}, el('dt', {}, label), el('dd', {}, value ?? '—', hint ? el('small', {}, hint) : null))));
}
// A small segmented control (radio-like buttons).
export function segmented(label, options, value, onChange, { disabled = false } = {}) {
  return el('div', { class: 'segmented', role: 'group', 'aria-label': label }, options.map(([key, text]) => {
    const node = button(text, () => onChange(key), 'segment' + (key === value ? ' selected' : ''), null, { 'aria-pressed': String(key === value), 'data-segment': key, 'data-focus': 'segment-' + key });
    node.disabled = disabled; return node;
  }));
}
export function select(label, options, value, onChange, { disabled = false, className = 'filter-select', attrs = {} } = {}) {
  return el('select', { class: className, 'aria-label': label, disabled, onchange: event => onChange(event.target.value), ...attrs }, options.map(([key, text]) => el('option', { value: key, selected: String(key) === String(value) }, text)));
}

// Time and number formatting. Every value shown keeps its unit.
const valid = value => { const time = typeof value === 'number' ? value : Date.parse(value); return Number.isFinite(time) ? time : null; };
const pad = n => String(n).padStart(2, '0');
// "2026. 10. 26. 14:59" (24-hour, local time): short and the same in every browser.
// English: "Nov 11, 2026, 14:59" (Intl, 24-hour).
const englishDate = new Intl.DateTimeFormat('en-US', { year: 'numeric', month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit', hourCycle: 'h23' });
export const date = value => { const time = valid(value); if (time === null) return '—'; const d = new Date(time); return lang === 'ko' ? `${d.getFullYear()}. ${d.getMonth() + 1}. ${d.getDate()}. ${pad(d.getHours())}:${pad(d.getMinutes())}` : englishDate.format(d); };
// "11월 11일" / "Nov 11": a day of this year.
export const monthDay = value => { const time = valid(value); return time === null ? '—' : new Date(time).toLocaleDateString(locale, { month: lang === 'ko' ? 'long' : 'short', day: 'numeric' }); };
export const clock = value => { const time = valid(value); return time === null ? '—' : new Date(time).toLocaleTimeString(locale, { hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit' }); };
export const hhmm = value => { const time = valid(value); return time === null ? '—' : new Date(time).toLocaleTimeString(locale, { hour12: false, hour: '2-digit', minute: '2-digit' }); };
export const day = value => { const time = valid(value); return time === null ? '—' : new Date(time).toLocaleDateString(locale, { year: 'numeric', month: 'long', day: 'numeric', weekday: 'short' }); };
export function relative(value, now = Date.now()) {
  const time = valid(value); if (time === null) return '—';
  const seconds = Math.round((now - time) / 1000);
  if (seconds < 0) return date(time);
  if (seconds < 45) return t('dom.3');
  if (seconds < 3600) return t('dom.4', { count: Math.max(1, Math.round(seconds / 60)) });
  if (seconds < 86400) return t('dom.5', { count: Math.round(seconds / 3600) });
  if (seconds < 86400 * 14) return t('dom.6', { count: Math.round(seconds / 86400) });
  return date(time);
}
// A time ahead: "50분 후", "3시간 후"; past or far times fall back to the date.
export function until(value, now = Date.now()) {
  const time = valid(value); if (time === null) return '—';
  const seconds = Math.round((time - now) / 1000);
  if (seconds <= 0) return date(time);
  if (seconds < 60) return t('dom.7');
  if (seconds < 3600) return t('dom.8', { count: Math.round(seconds / 60) });
  if (seconds < 86400) return t('dom.9', { count: Math.round(seconds / 3600) });
  if (seconds < 86400 * 14) return t('dom.10', { count: Math.round(seconds / 86400) });
  return date(time);
}
// "1시간 36분 (16:35부터)" / "1 h 36 min (since 16:35)": how long something has lasted and when it began (the date when not today).
export function sinceText(value, now = Date.now()) {
  const time = valid(value); if (time === null) return '—';
  const start = new Date(time), today = new Date(now), sameDay = start.toDateString() === today.toDateString();
  return t('dom.since', { duration: duration(now - time), time: sameDay ? hhmm(time) : date(time) });
}
export const timeTag = (value, text = relative(value)) => el('time', { datetime: valid(value) === null ? null : new Date(valid(value)).toISOString(), title: date(value) }, text);
export function duration(ms) {
  if (!Number.isFinite(ms) || ms < 0) return '—';
  const minutes = Math.round(ms / 60000);
  if (minutes < 1) return t('dom.11', { count: Math.max(1, Math.round(ms / 1000)) });
  if (minutes < 60) return t('common.101', { minutes });
  return t('dom.12', { count: Math.floor(minutes / 60), value: minutes % 60 });
}
export const fixed = (value, digits = 1) => Number.isFinite(value) ? value.toLocaleString(locale, { minimumFractionDigits: digits, maximumFractionDigits: digits }) : '—';
export const integer = value => Number.isFinite(value) ? Math.round(value).toLocaleString(locale) : '—';
// Microseconds as milliseconds (the client reports times in µs).
export const ms = us => !Number.isFinite(us) ? '—' : us >= 100000 ? `${integer(us / 1000)} ms` : `${fixed(us / 1000, us >= 10000 ? 0 : 1)} ms`;
export const kbps = value => !Number.isFinite(value) ? '—' : value >= 1000 ? `${fixed(value / 1000, 1)} Mbps` : `${integer(value)} kbps`;
export function bytes(value) {
  if (!Number.isFinite(value)) return '—';
  if (value >= 1024 * 1024) return fixed(value / (1024 * 1024), 1) + ' MB';
  return Math.max(1, Math.round(value / 1024)) + ' KB';
}
