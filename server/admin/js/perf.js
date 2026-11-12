// Streaming performance windows (stream-stats-v1, apps/windows_client/lib/
// data/stream_stats_report.dart): one row per ~30 s window; fps values are
// x10, times are microseconds, rates kbps. These are client-reported
// observations, summarised here without inventing missing values.
import { el, fixed, integer, ms, kbps, clock, hhmm, date, monthDay, duration, table, facts } from './dom.js';
import { t, lang } from './i18n.js';

// "10월 26일 08:02" (today: "08:02").
const dayTime = value => { const d = new Date(value); return new Date().toDateString() === d.toDateString() ? hhmm(value) : (lang === 'ko' ? t('perf.1', { getMonth: d.getMonth() + 1, getDate: d.getDate(), hhmm: hhmm(value) }) : `${monthDay(value)} ${hhmm(value)}`); };
import { lineChart, stackedBar } from './charts.js';

const finite = value => Number.isFinite(value) ? value : null;
// Mean weighted by each window's 1 s sample count.
function mean(windows, key, scale = 1) {
  let sum = 0, weight = 0;
  for (const row of windows) { const value = finite(row.values[key]); if (value === null) continue; const w = Math.max(1, row.values.samples ?? 1); sum += value * w; weight += w; }
  return weight ? sum / weight / scale : null;
}
const extreme = (windows, key, pick, scale = 1) => { const values = windows.map(row => finite(row.values[key])).filter(value => value !== null); return values.length ? pick(...values) / scale : null; };
const latest = (windows, key) => { for (let i = windows.length - 1; i >= 0; i--) { const value = finite(windows[i].values[key]); if (value !== null) return value; } return null; };

export function summarize(session) {
  const windows = session.windows, first = windows[0], last = windows[windows.length - 1];
  const end = Date.parse(last.time), start = Date.parse(first.time) - (first.values.windowMs ?? 0);
  return {
    start, end, windows: windows.length,
    width: latest(windows, 'width'), height: latest(windows, 'height'), srcWidth: latest(windows, 'srcWidth'), srcHeight: latest(windows, 'srcHeight'),
    displayed: mean(windows, 'textureFps10', 10), decoded: mean(windows, 'decodeFps10', 10), decodedMin: extreme(windows, 'decodeFpsMin10', Math.min, 10),
    captured: mean(windows, 'captureFps10', 10), encoded: mean(windows, 'encodeFps10', 10),
    rtt: mean(windows, 'rttMs'), rttMax: extreme(windows, 'rttMaxMs', Math.max), networkDelay: mean(windows, 'networkDelayMs'),
    hostWork: mean(windows, 'hostWorkUs'), hostP95: extreme(windows, 'hostP95Us', Math.max), capture: mean(windows, 'captureUs'), convert: mean(windows, 'convertUs'), encode: mean(windows, 'encodeUs'),
    decode: mean(windows, 'decodeUs'), decodeMax: extreme(windows, 'decodeMaxUs', Math.max), queueMax: extreme(windows, 'queueMaxMs', Math.max),
    received: mean(windows, 'receiveKbps'), bitrate: latest(windows, 'bitrateKbps'), target: latest(windows, 'targetKbps'), auto: windows.some(row => row.values.auto === true),
    skipped: extreme(windows, 'skipped', Math.max), lossless: extreme(windows, 'losslessTiles', Math.max),
    inputHostP50: latest(windows, 'inputHostP50Us'), inputHostP95: latest(windows, 'inputHostP95Us'), inputFrameP50: latest(windows, 'inputFrameP50Us'), inputFrameP95: latest(windows, 'inputFrameP95Us'),
    // From the Host: the slowest single frame of each stage; from the client:
    // decoded -> shown wait, texture upload and the client's display.
    topologyMax: extreme(windows, 'topologyMaxUs', Math.max), captureMax: extreme(windows, 'captureMaxUs', Math.max),
    convertMax: extreme(windows, 'convertMaxUs', Math.max), encodeMax: extreme(windows, 'encodeMaxUs', Math.max),
    textureWait: mean(windows, 'textureWaitUs'), textureUpload: mean(windows, 'textureUploadUs'), dropped: extreme(windows, 'skippedFrames', Math.max),
    displayHz: latest(windows, 'displayHz'), gpuHardware: [...windows].reverse().map(row => row.values.gpuHardware).find(value => typeof value === 'boolean') ?? null,
    remoteSession: windows.some(row => row.values.remoteSession === true),
  };
}
const stageMaxima = s => [['topologyMax', t('perf.2')], ['captureMax', t('perf.3')], ['convertMax', t('perf.4')], ['encodeMax', t('perf.5')]].filter(([key]) => s[key] !== null);

const fps = value => value === null ? '—' : `${fixed(value, 1)} fps`;
export const fpsText = fps;
const resolution = (w, h) => w && h ? `${w}×${h}` : null;
// Per-frame budget of the product's 60 fps target, used only as a reference line.
const budgetUs = 1e6 / 60;

// Observations, not a diagnosis: each line names the measured values it rests on.
export function findings(s) {
  const out = [];
  const stages = [['capture', t('perf.3'), s.capture], ['convert', t('perf.4'), s.convert], ['encode', t('perf.5'), s.encode], ['decode', t('perf.6'), s.decode]].filter(([, , value]) => value !== null);
  if (stages.length) {
    const [, name, value] = stages.reduce((a, b) => b[2] > a[2] ? b : a);
    out.push({ tone: value > budgetUs ? 'warn' : 'info', text: t('perf.8', { name, ms: ms(value), value: value > budgetUs ? t('perf.7', { ms: ms(budgetUs) }) : '' }) });
  }
  if (s.hostWork !== null && s.hostWork > budgetUs) out.push({ tone: 'warn', text: t('perf.9', { ms: ms(s.hostWork) }) });
  // Which stage the slow Host frames came from (single slowest frame per stage).
  const maxima = stageMaxima(s);
  if (s.hostP95 !== null && s.hostP95 > 2 * budgetUs && maxima.length) {
    const [key, name] = maxima.reduce((a, b) => s[b[0]] > s[a[0]] ? b : a);
    out.push({ tone: 'warn', text: t('perf.10', { ms: ms(s.hostP95), name, ms2: ms(s[key]) }) });
  }
  if (s.textureWait !== null && s.textureWait > 8000) out.push({ tone: 'info', text: t('perf.12', { ms: ms(s.textureWait), value: s.displayHz ? t('perf.11', { integer: integer(s.displayHz) }) : '' }) });
  if (s.remoteSession) out.push({ tone: 'info', text: t('perf.13') });
  else if (s.gpuHardware === false) out.push({ tone: 'info', text: t('perf.14') });
  if (s.captured !== null && s.decoded !== null && s.captured >= 5 && s.decoded < s.captured * 0.85) out.push({ tone: 'warn', text: t('perf.15', { fps: fps(s.captured), fps2: fps(s.decoded) }) });
  if (s.rtt !== null && s.rtt >= 80) out.push({ tone: 'warn', text: t('perf.16', { integer: integer(s.rtt) }) });
  if (s.inputFrameP95 !== null && s.inputFrameP95 >= 100000) out.push({ tone: 'warn', text: t('perf.17', { ms: ms(s.inputFrameP95) }) });
  if (s.captured !== null && s.captured < 20) out.push({ tone: 'info', text: t('perf.18', { fps: fps(s.captured) }) });
  return out;
}

const stageSegments = s => [['capture', t('perf.3'), 's1'], ['convert', t('perf.4'), 's2'], ['encode', t('perf.5'), 's3'], ['decode', t('perf.19'), 's4']].map(([key, label, tone]) => ({ label, value: s[key] ?? 0, tone }));

export function sessionHeadline(session, desktop) {
  const s = summarize(session);
  return { s, title: [session.account ?? t('common.71'), desktop ? `${desktop.name} (VM ${desktop.pve_vmid})` : session.desktopId ?? null].filter(Boolean).join(' · '),
    meta: [`${dayTime(s.start)}–${hhmm(s.end)} (${duration(s.end - s.start)})`, resolution(s.width, s.height)].filter(Boolean).join(' · ') };
}

// Key numbers as a compact metric row (used in session summaries and cards).
export function metricRow(s) {
  const item = (label, value, detail) => el('div', { class: 'metric' }, el('span', {}, label), el('strong', {}, value), detail ? el('small', {}, detail) : null);
  return el('div', { class: 'metric-row' },
    item(t('perf.20'), fps(s.displayed ?? s.decoded), s.decodedMin === null ? null : t('perf.21', { fps: fps(s.decodedMin) })),
    item(t('perf.22'), fps(s.captured), s.encoded === null ? null : t('perf.23', { fps: fps(s.encoded) })),
    item(t('perf.24'), s.rtt === null ? '—' : `${integer(s.rtt)} ms`, s.rttMax === null ? null : t('perf.25', { integer: integer(s.rttMax) })),
    item(t('perf.26'), ms(s.inputFrameP50), s.inputFrameP95 === null ? null : `p95 ${ms(s.inputFrameP95)}`),
    item(t('common.144'), kbps(s.received), s.target === null ? null : t('perf.28', { kbps: kbps(s.target), value: s.auto ? t('perf.27') : '' })));
}

// Full performance view of one streaming session.
export function sessionDetail(session) {
  const s = summarize(session), w = session.windows;
  const point = (key, scale = 1) => w.map(row => ({ t: Date.parse(row.time), v: Number.isFinite(row.values[key]) ? row.values[key] / scale : null }));
  const notes = findings(s);
  const rows = w.map(row => { const v = row.values; const f = key => Number.isFinite(v[key]) ? v[key] : null;
    return [clock(row.time), fps(f('textureFps10') === null ? (f('decodeFps10') === null ? null : f('decodeFps10') / 10) : f('textureFps10') / 10), fps(f('captureFps10') === null ? null : f('captureFps10') / 10),
      ms(f('captureUs')), ms(f('convertUs')), ms(f('encodeUs')), ms(f('decodeUs')), f('rttMs') === null ? '—' : `${integer(f('rttMs'))} ms`,
      f('inputFrameP50Us') === null ? '—' : `${ms(f('inputFrameP50Us'))} / ${ms(f('inputFrameP95Us'))}`, kbps(f('receiveKbps'))]; });
  return el('div', { class: 'perf-detail' },
    notes.length ? el('ul', { class: 'findings' }, notes.map(note => el('li', { class: 'is-' + note.tone }, note.text))) : null,
    el('div', { class: 'chart-grid' },
      lineChart({ label: t('perf.29'), points: point(w.some(row => Number.isFinite(row.values.textureFps10)) ? 'textureFps10' : 'decodeFps10', 10), format: value => `${fixed(value, value >= 100 ? 0 : 1)}`, maxHint: 60 }),
      lineChart({ label: t('perf.30'), points: point('inputFrameP50Us', 1000), format: value => `${fixed(value, value >= 100 ? 0 : 1)}` }),
      lineChart({ label: t('perf.31'), points: point('rttMs'), format: value => `${fixed(value, 0)}` }),
      lineChart({ label: t('perf.32'), points: point('receiveKbps', 1000), format: value => `${fixed(value, 1)}` })),
    el('div', { class: 'perf-stages' }, el('h4', {}, t('perf.33')),
      stackedBar({ label: null, segments: stageSegments(s), format: ms, showPercent: false }),
      el('p', { class: 'field-hint' }, t('perf.34', { ms: ms(budgetUs) }))),
    facts([
      session.version ? [t('perf.clientVersion'), session.version] : null,
      [t('perf.35'), [resolution(s.srcWidth, s.srcHeight) ?? '—', ' → ', resolution(s.width, s.height) ?? '—'].join('')],
      [t('perf.36'), `${ms(s.hostWork)} / ${ms(s.hostP95)}`],
      ...(stageMaxima(s).length ? [[t('perf.37'), stageMaxima(s).map(([key, name]) => `${name} ${ms(s[key])}`).join(' · ')]] : []),
      ...(s.textureWait !== null || s.textureUpload !== null ? [[t('perf.38'), `${ms(s.textureWait)} · ${ms(s.textureUpload)}`]] : []),
      ...(s.displayHz !== null || s.gpuHardware !== null ? [[t('perf.39'), [s.displayHz === null ? null : `${integer(s.displayHz)} Hz`, s.gpuHardware === null ? null : s.gpuHardware ? t('perf.40') : t('perf.41')].filter(Boolean).join(' · ') + (s.remoteSession ? ` (${t('perf.42')})` : '')]] : []),
      [t('perf.43'), `${ms(s.decodeMax)} · ${s.queueMax === null ? '—' : integer(s.queueMax) + ' ms'}`],
      [t('perf.44'), `${ms(s.inputHostP50)} / ${ms(s.inputHostP95)}`],
      [t('perf.45'), kbps(s.bitrate)],
      [t('perf.46'), `${s.skipped === null ? '—' : integer(s.skipped)}${s.dropped === null ? '' : t('perf.47', { integer: integer(s.dropped) })} · ${s.lossless === null ? '—' : integer(s.lossless)}`],
    ], 'facts-grid'),
    el('details', { class: 'table-view', 'data-keep-open': 'perf-table:' + (session.sessionId ?? session.key) }, el('summary', {}, t('perf.48', { wCount: w.length })),
      table([t('perf.49'), t('perf.20'), t('perf.3'), t('perf.50'), t('perf.4'), t('perf.5'), t('perf.19'), t('perf.51'), t('perf.52'), t('common.144')], rows, t('perf.53'), '', { className: 'perf-table' })),
    el('p', { class: 'field-hint' }, t('perf.54', { date: date(w[w.length - 1].receivedAt) })));
}
