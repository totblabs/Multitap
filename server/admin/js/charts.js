// Small library-free charts drawn as SVG. Plot geometry stretches with the
// card (preserveAspectRatio="none"); strokes keep their width through
// vector-effect and dots are zero-length round-capped lines, so nothing
// distorts. Text stays in HTML around the plot, colours come from console
// tokens (classes), and every chart offers a readout plus a table view.
import { el, svg, clock, emptyState } from './dom.js';
import { t } from './i18n.js';

const W = 1000, H = 100, TOP = 6;
function niceMax(value) {
  if (!(value > 0)) return 1;
  const power = 10 ** Math.floor(Math.log10(value)), scaled = value / power;
  return (scaled <= 1 ? 1 : scaled <= 2 ? 2 : scaled <= 2.5 ? 2.5 : scaled <= 5 ? 5 : 10) * power;
}

// One series over time: [{ t: ms, v: number|null }]. `format` renders a value
// with its unit. Hover, focus and arrow keys move one readout.
export function lineChart({ label, points, format, maxHint = 0, className = '' }) {
  const usable = points.filter(point => Number.isFinite(point.t) && Number.isFinite(point.v));
  if (!usable.length) return el('figure', { class: 'chart is-empty ' + className }, el('figcaption', { class: 'chart-head' }, el('span', { class: 'chart-title' }, label)), emptyState(t('common.38'), t('charts.1'), { compact: true, iconName: 'activity' }));
  const values = usable.map(point => point.v);
  const max = niceMax(Math.max(...values, maxHint)), start = usable[0].t, end = usable[usable.length - 1].t;
  const x = t => end === start ? W / 2 : (t - start) / (end - start) * W;
  const y = v => TOP + (1 - Math.min(v, max) / max) * (H - TOP);
  // Gaps (missing values) break the line instead of inventing a slope.
  const segments = []; let current = [];
  for (const point of points) {
    if (Number.isFinite(point.t) && Number.isFinite(point.v)) current.push(point); else if (current.length) { segments.push(current); current = []; }
  }
  if (current.length) segments.push(current);
  const line = segments.map(segment => 'M' + segment.map(point => `${x(point.t).toFixed(1)},${y(point.v).toFixed(1)}`).join('L')).join('');
  const area = segments.filter(segment => segment.length > 1).map(segment => `M${x(segment[0].t).toFixed(1)},${H}` + segment.map(point => `L${x(point.t).toFixed(1)},${y(point.v).toFixed(1)}`).join('') + `L${x(segment[segment.length - 1].t).toFixed(1)},${H}Z`).join('');
  const last = usable[usable.length - 1];
  const dot = (point, cls) => svg('line', { class: cls, x1: x(point.t).toFixed(1), x2: x(point.t).toFixed(1), y1: y(point.v).toFixed(1), y2: y(point.v).toFixed(1) });
  const cross = svg('line', { class: 'chart-cross', x1: 0, x2: 0, y1: 0, y2: H, hidden: true });
  const hoverDot = svg('line', { class: 'chart-dot is-hover', x1: 0, x2: 0, y1: 0, y2: 0, hidden: true });
  const plot = svg('svg', { class: 'chart-plot', viewBox: `0 0 ${W} ${H}`, preserveAspectRatio: 'none', 'aria-hidden': 'true', focusable: 'false' },
    [0, 0.5, 1].map(f => svg('line', { class: 'chart-grid', x1: 0, x2: W, y1: (TOP + f * (H - TOP)).toFixed(1), y2: (TOP + f * (H - TOP)).toFixed(1) })),
    area ? svg('path', { class: 'chart-area', d: area }) : null, svg('path', { class: 'chart-line', d: line }),
    usable.length === 1 ? dot(last, 'chart-dot') : dot(last, 'chart-dot is-end'), cross, hoverDot);
  const readout = el('span', { class: 'chart-readout', 'aria-live': 'polite' });
  const show = point => {
    readout.replaceChildren(el('strong', {}, format(point.v)), ' ', el('small', {}, point === last ? t('common.39') : clock(point.t)));
  };
  show(last);
  const move = point => {
    const px = x(point.t).toFixed(1), py = y(point.v).toFixed(1);
    for (const [node, attrs] of [[cross, { x1: px, x2: px }], [hoverDot, { x1: px, x2: px, y1: py, y2: py }]]) { for (const [k, v] of Object.entries(attrs)) node.setAttribute(k, v); node.removeAttribute('hidden'); }
    show(point);
  };
  const reset = () => { cross.setAttribute('hidden', ''); hoverDot.setAttribute('hidden', ''); show(last); };
  let index = usable.length - 1;
  const nearest = ratio => { let best = 0; for (let i = 1; i < usable.length; i++) if (Math.abs(x(usable[i].t) / W - ratio) < Math.abs(x(usable[best].t) / W - ratio)) best = i; return best; };
  const frame = el('div', { class: 'chart-frame', tabindex: '0', role: 'img', 'aria-label': t('charts.2', { label, format: format(Math.min(...values)), format2: format(Math.max(...values)), format3: format(last.v) }),
    onpointermove: event => { const rect = plot.getBoundingClientRect?.(); if (!rect?.width) return; index = nearest((event.clientX - rect.left) / rect.width); move(usable[index]); },
    onpointerleave: reset, onblur: reset,
    onkeydown: event => {
      const next = { ArrowLeft: index - 1, ArrowRight: index + 1, Home: 0, End: usable.length - 1 }[event.key];
      if (next === undefined) return;
      event.preventDefault(); index = Math.max(0, Math.min(usable.length - 1, next)); move(usable[index]);
    } },
    el('div', { class: 'chart-y', 'aria-hidden': 'true' }, el('span', {}, format(max)), el('span', {}, format(max / 2)), el('span', {}, format(0))), plot);
  return el('figure', { class: 'chart ' + className },
    el('figcaption', { class: 'chart-head' }, el('span', { class: 'chart-title' }, label), readout), frame,
    el('div', { class: 'chart-x', 'aria-hidden': 'true' }, el('span', {}, clock(start)), el('span', {}, end === start ? '' : clock(end))));
}

// Part-to-whole bar with a legend that carries every value (the legend is the
// readable form; segment titles add a hover readout).
export function stackedBar({ label, segments, format = value => String(value), total = null, showPercent = true }) {
  const sum = total ?? segments.reduce((value, segment) => value + (Number.isFinite(segment.value) ? segment.value : 0), 0);
  let offset = 0;
  const rects = sum > 0 ? segments.filter(segment => segment.value > 0).map(segment => {
    const width = segment.value / sum * W, node = svg('rect', { class: 'stack-seg c-' + segment.tone, x: offset.toFixed(2), y: 0, width: width.toFixed(2), height: 10 }, svg('title', {}, `${segment.label} ${format(segment.value)}`));
    offset += width; return node;
  }) : [];
  return el('figure', { class: 'stack' },
    label ? el('figcaption', { class: 'stack-label' }, label) : null,
    el('div', { class: 'stack-track' + (sum > 0 ? '' : ' is-empty'), role: 'img', 'aria-label': `${label ?? ''} ${segments.map(segment => `${segment.label} ${format(segment.value)}`).join(', ')}` },
      svg('svg', { class: 'stack-svg', viewBox: `0 0 ${W} 10`, preserveAspectRatio: 'none', 'aria-hidden': 'true', focusable: 'false' }, rects)),
    el('ul', { class: 'legend' }, segments.map(segment => el('li', {}, el('span', { class: 'swatch c-' + segment.tone, 'aria-hidden': 'true' }), el('span', { class: 'legend-label' }, segment.label), el('strong', {}, format(segment.value)),
      showPercent && sum > 0 && Number.isFinite(segment.value) ? el('small', {}, `${Math.round(segment.value / sum * 100)}%`) : null))));
}

// Ranked horizontal bars, longest first: one row per item with its name, its
// bar and its exact value as text, so the numbers never depend on the bar.
// With `onSelect` each row is a button (aria-pressed marks the chosen one).
export function rankBars({ label, rows, format, selected = null, onSelect = null, focusKey = 'rank' }) {
  const max = Math.max(0, ...rows.map(row => Number.isFinite(row.value) ? row.value : 0));
  return el('ol', { class: 'rank-bars', 'aria-label': label }, rows.map(row => {
    // A value above zero always shows a sliver.
    const width = max > 0 && row.value > 0 ? Math.max(4, row.value / max * W) : 0;
    const body = [el('span', { class: 'rank-name' }, row.name),
      el('span', { class: 'rank-track', 'aria-hidden': 'true' }, svg('svg', { class: 'rank-svg', viewBox: `0 0 ${W} 10`, preserveAspectRatio: 'none', focusable: 'false' }, svg('rect', { class: 'rank-fill', x: 0, y: 0, width: width.toFixed(1), height: 10 }))),
      el('span', { class: 'rank-value' }, el('strong', {}, format(row.value)), row.note ? el('small', {}, row.note) : null)];
    return el('li', {}, onSelect ? el('button', { type: 'button', class: 'rank-row' + (selected === row.key ? ' selected' : ''), 'aria-pressed': String(selected === row.key), 'data-focus': `${focusKey}-${row.key}`, onclick: () => onSelect(row.key) }, body)
      : el('div', { class: 'rank-row' }, body));
  }));
}

// One bar per day: [{ label, value, note }] in time order. Hover, focus and
// the arrow keys move one readout; the table view of the page lists every day.
export function dayBars({ label, days, format, className = '', nice = niceMax }) {
  const values = days.map(day => Number.isFinite(day.value) ? day.value : 0), top = Math.max(0, ...values);
  if (!days.length || !(top > 0)) return el('figure', { class: 'chart is-empty ' + className }, el('figcaption', { class: 'chart-head' }, el('span', { class: 'chart-title' }, label)), emptyState(t('common.40'), t('charts.3'), { compact: true, iconName: 'activity' }));
  const max = nice(top), slot = 10, bar = days.length <= 7 ? 3 : days.length <= 14 ? 4.5 : days.length <= 31 ? 6 : 7.5, width = days.length * slot;
  const y = value => TOP + (1 - Math.min(value, max) / max) * (H - TOP);
  const rects = days.map((day, index) => values[index] > 0 ? svg('rect', { class: 'day-bar', x: (index * slot + (slot - bar) / 2).toFixed(2), y: y(values[index]).toFixed(1), width: bar, height: (H - y(values[index])).toFixed(1) }) : null);
  const marker = svg('rect', { class: 'day-marker', x: 0, y: 0, width: slot, height: H, hidden: true });
  const plot = svg('svg', { class: 'chart-plot day-plot', viewBox: `0 0 ${width} ${H}`, preserveAspectRatio: 'none', 'aria-hidden': 'true', focusable: 'false' },
    [0, 0.5, 1].map(f => svg('line', { class: 'chart-grid', x1: 0, x2: width, y1: (TOP + f * (H - TOP)).toFixed(1), y2: (TOP + f * (H - TOP)).toFixed(1) })), marker, rects);
  const readout = el('span', { class: 'chart-readout', 'aria-live': 'polite' });
  const best = values.indexOf(top);
  const show = (index, resting = false) => readout.replaceChildren(el('strong', {}, format(values[index])), ' ', el('small', {}, [resting ? t('charts.4') : '', days[index].label, days[index].note ? ' · ' + days[index].note : ''].join('')));
  const move = index => { marker.setAttribute('x', String(index * slot)); marker.removeAttribute('hidden'); for (const [i, node] of rects.entries()) node?.setAttribute('class', 'day-bar' + (i === index ? ' is-hover' : '')); show(index); };
  const reset = () => { marker.setAttribute('hidden', ''); for (const node of rects) node?.setAttribute('class', 'day-bar'); show(best, true); };
  show(best, true);
  let index = best;
  const frame = el('div', { class: 'chart-frame', tabindex: '0', role: 'img', 'aria-label': t('charts.5', { label, daysCount: days.length, value: days[best].label, format: format(top) }),
    onpointermove: event => { const rect = plot.getBoundingClientRect?.(); if (!rect?.width) return; index = Math.max(0, Math.min(days.length - 1, Math.floor((event.clientX - rect.left) / rect.width * days.length))); move(index); },
    onpointerleave: reset, onblur: reset,
    onkeydown: event => {
      const next = { ArrowLeft: index - 1, ArrowRight: index + 1, Home: 0, End: days.length - 1 }[event.key];
      if (next === undefined) return;
      event.preventDefault(); index = Math.max(0, Math.min(days.length - 1, next)); move(index);
    } },
    el('div', { class: 'chart-y', 'aria-hidden': 'true' }, el('span', {}, format(max)), el('span', {}, format(max / 2)), el('span', {}, format(0))), plot);
  return el('figure', { class: 'chart day-chart ' + className },
    el('figcaption', { class: 'chart-head' }, el('span', { class: 'chart-title' }, label), readout), frame,
    el('div', { class: 'chart-x', 'aria-hidden': 'true' }, el('span', {}, days[0].label), el('span', {}, days.length > 1 ? days.at(-1).label : '')));
}
