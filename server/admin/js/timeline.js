// One activity timeline from three independent sources: administrator and
// server audit records (inventory), Windows client diagnostics and Host
// diagnostics events. Each entry keeps its source; times are as reported by
// that source (client/Host PCs use their own clocks).
import { el, button, stateDot, facts, board, detailWindow, clock, day, date, emptyState, fixed, integer, ms } from './dom.js';
import { own, clientEvents, clientCodes, hostEvents, hostActions, hostClipboardWin32Text, hostUpdateEventStates, hostUpdateEventCodes, hostUpdateStateText, valueNames, eventLabel, auditFailures, hostUpdateCodeText, hostMediaDropText, hostPrintText, hostPrintDefaultReasonText, hostDriveText, hostDriveErrorText, hostChannelCloseCodes, hostChannelCloseText, hostUsbText, usbControllerCodes, usbControllerText, hostDisplayInventoryText, hostUsbReasons, usbRequestClassText, usbErrnoLabel, clientUsbText, clientUsbBack, hostClipboardStageText, hostClipboardFormatText, hostClipboardNoteText, hostClipboardFormatBits, hostClipboardReasons, hostClipboardCauses, hostClipboardCauseText, hostConsoleParkText, hostConsoleParkRefusalText, hostConsoleParkCauseText, hostInputKindText, hostInputLevelText, hostInputInjectorReasonText, hostFinishStepText, hostFinishDisplayCodes, hostFinishWinfspCodes, hostFinishCodeText, hostRemovalReasons, hostRemovalReasonText, hostRemovalCertificates, hostRemovalCertificateText, hostUdpProbeText, hostUdpPathText, hostUdpPathReasonText, hostThumbnailText, hostBootStages, failureAdvice, consoleProofCodeText } from './labels.js';
import { t } from './i18n.js';

const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const powerActions = { start: t('common.150'), shutdown: t('timeline.1'), reboot: t('common.46'), stop: t('timeline.2') };
// Automatic bookkeeping of every Proxmox synchronisation; hidden by default.
export const routineAudit = e => e.event === 'proxmox_synchronized' || (e.event === 'integration_updated' && ['proxmox_sync', 'proxmox_token_status'].includes(e.subject));
const settingNames = { proxmox: t('timeline.3'), proxmox_sync: t('timeline.4'), proxmox_token_status: t('timeline.5'), proxmox_initialized: t('timeline.6') };

export function auditEntry(e, usersById, desktopsById) {
  const entry = { key: 'audit:' + e.id, time: e.created_at, source: 'audit', event: e.event, title: eventLabel(e.event), detail: [], failure: auditFailures.has(e.event), desktopId: null, userId: null, account: null, routine: routineAudit(e) };
  const actor = usersById.get(e.actor);
  entry.actor = actor ? actor.username : ['host_enrolled', 'host_channel_closed', 'host_machine_changed'].includes(e.event) ? 'Host' : ['server-admin', 'server'].includes(e.actor) ? t('common.146') : ['unverified-host', 'request-owner'].includes(e.actor) ? t('timeline.7') : uuid.test(e.actor) ? t('common.71') : e.actor;
  entry.actorId = actor?.id ?? null;
  let tokens = [];
  if (e.subject?.startsWith('{')) { try { const value = JSON.parse(e.subject); tokens = [value.desktopId]; if (value.mode) entry.detail.push(t('common.67')); if (value.status) entry.detail.push(String(value.status)); } catch { tokens = []; } }
  else tokens = String(e.subject ?? '').split(':');
  for (const token of tokens) {
    if (!entry.desktopId && desktopsById.has(token)) entry.desktopId = token;
    else if (!entry.userId && usersById.has(token)) { entry.userId = token; entry.account = usersById.get(token).username; }
  }
  const rest = tokens.slice(1);
  if (e.event.startsWith('host_update_') && rest[0]?.includes('>')) { const [from, to] = rest[0].split('>'); entry.detail.push(`${from} → ${to}`); if (rest[1] && rest[1] !== '-' && rest[1] !== 'admin' && rest[1] !== 'client') { entry.code = rest[1]; entry.codeLabel = own(hostUpdateCodeText, rest[1]); } if (rest[1] === 'client') entry.detail.push(t('timeline.8')); if (rest.includes('disconnect')) entry.detail.push(t('timeline.9')); if (rest.includes('restart_required')) entry.detail.push(t('timeline.10')); }
  else if (e.event.startsWith('power_') && own(powerActions, rest[0])) entry.detail.push(own(powerActions, rest[0]));
  else if (e.event === 'display_requested' && rest[0]) entry.detail.push(t('timeline.11', { value: rest[0] }));
  // desktopId:reason:detail:ended sessions (server/src/relay.mjs dropHost).
  else if (e.event === 'host_channel_closed') { entry.code = rest[1] || rest[0] || null; entry.codeLabel = entry.code ? own(clientCodes, entry.code) ?? null : null; if (Number(rest[2]) > 0) entry.detail.push(t('timeline.12', { Number: Number(rest[2]) })); }
  // desktopId:code (server/src/relay.mjs auditConsoleProof).
  else if (['stream_console_unverified', 'stream_console_identity_refused'].includes(e.event) && rest[0]) { entry.code = rest[0]; entry.codeLabel = own(consoleProofCodeText, rest[0]) ?? null; if (entry.codeLabel) entry.detail.push(entry.codeLabel); }
  else if (['display_completed', 'console_resume_completed'].includes(e.event) && rest.length) { entry.code = rest.filter(Boolean).join(':'); if (e.event === 'display_completed' && rest[0] && rest[0] !== 'succeeded' && rest[0] !== 'completed') entry.failure = true; }
  else if (e.event === 'assignments_replaced') { const added = rest.find(t => t.startsWith('+')), removed = rest.find(t => t.startsWith('-')); entry.detail.push(t('timeline.13', { slice: added?.slice(1) ?? 0, slice2: removed?.slice(1) ?? 0 })); }
  // hostId:desktopId:count, desktopId:count and count (server/src/store.mjs): how many assignments went with the Host.
  else if (e.event === 'host_registration_removed' && /^\d+$/.test(rest[1] ?? '')) entry.detail.push(t('timeline.14', { Number: Number(rest[1]) }));
  else if (e.event === 'desktop_assignments_removed' && /^\d+$/.test(rest[0] ?? '')) entry.detail.push(t('timeline.14', { Number: Number(rest[0]) }));
  // desktopId:oldNode:oldVmid:newNode:newVmid (server/src/store.mjs relocateRegisteredVm).
  else if (e.event === 'desktop_vm_relocated' && rest.length === 4 && /^\d+$/.test(rest[1]) && /^\d+$/.test(rest[3])) entry.detail.push(t('timeline.vmRelocated', { fromNode: rest[0], fromVmid: rest[1], toNode: rest[2], toVmid: rest[3] }));
  else if (e.event === 'hostless_assignments_removed' && /^\d+$/.test(tokens[0] ?? '')) entry.detail.push(t('timeline.14', { Number: Number(tokens[0]) }));
  // operationId:people:connections (server/src/http.mjs, a save of the server access settings).
  else if (e.event === 'server_access_connections_ended' && /^\d+$/.test(rest[0] ?? '') && /^\d+$/.test(rest[1] ?? '')) entry.detail.push(t('timeline.15', { Number: Number(rest[0]), Number2: Number(rest[1]) }));
  else if (e.event === 'integration_updated') entry.detail.push(own(settingNames, e.subject) ?? e.subject);
  else if (e.event === 'proxmox_synchronized') entry.detail.push(t('timeline.16', { subject: e.subject }));
  // threshold:expiry date:fingerprint prefix (certificate-expiry.mjs).
  else if (e.event === 'tls_certificate_expiring') entry.detail.push(t('timeline.17', { value: tokens[0] }), t('timeline.18', { value: tokens[1] ?? t('common.17') }));
  else if (e.event === 'login_failed' && e.actor) entry.detail.push(t('timeline.19', { actor: e.actor }));
  // userId:count (failed codes within one minute) and userId:minutes | userId:day (server/src/mfa.mjs).
  else if (e.event === 'mfa_failed' && /^\d{1,6}$/.test(rest[0] ?? '')) entry.detail.push(t('timeline.mfaFailed', { count: Number(rest[0]) }));
  else if (e.event === 'mfa_locked' && rest[0] === 'day') entry.detail.push(t('timeline.mfaLockedDay'));
  else if (e.event === 'mfa_locked' && /^\d{1,3}$/.test(rest[0] ?? '')) entry.detail.push(t('timeline.mfaLockedMinutes', { minutes: Number(rest[0]) }));
  return entry;
}
// Virtual display adapter inventory and cleanup (hostAction 273, labels.js hostDisplayInventoryText).
function displayInventoryDetail(v) {
  const stage = hostDisplayInventoryText[v.stage] ?? t('timeline.20', { stage: v.stage });
  return v.stage === 0 ? [stage, t('timeline.21', { value: v.expected ?? 0 }), t('timeline.22', { value: v.stale ?? 0 }), t('timeline.23', { value: v.missingOwned ?? 0 }), t('timeline.24', { value: v.fallback ?? 0 }), t('timeline.25', { value: v.items ?? 0 }), v.ok ? t('timeline.26') : null]
    : [stage, t('timeline.27', { value: v.items ?? 0 }), t('timeline.28', { value: v.missing ?? 0 }), t('timeline.29', { value: v.error ?? 0 })];
}
// One service start's way to 연결 준비됨 (hostAction 274, labels.js hostBootStages): when the service started
// after Windows did, then each stage reached, in seconds after the service start.
export function bootReadyDetail(v) {
  const seconds = value => t('timeline.30', { value: (value / 1000).toFixed(1) });
  return [v.ok ? t('timeline.31') : t('timeline.32'), Number.isFinite(v.uptimeMs) ? t('timeline.33', { seconds: seconds(v.uptimeMs) }) : null,
    ...hostBootStages.filter(([key]) => Number.isFinite(v[key])).map(([key, label]) => `${label} +${seconds(v[key])}`)];
}
// Codes and identifiers (error numbers, stages) are shown without digit grouping.
// Streaming USB redirection on the desktop (hostAction 250): the stage, then what the numbers of that stage mean (labels.js hostUsbText).
export function usbHostDetail(v) {
  const stage = hostUsbText[v.stage] ?? t('timeline.20', { stage: v.stage });
  const reason = hostUsbReasons[v.error - 1];
  const request = v.expected > 0 && [17, 24, 25, 26].includes(v.stage) ? usbRequestClassText(v.expected) : null;
  switch (v.stage) {
    case 8: return [stage, Number.isInteger(v.expected) ? t('timeline.34', { value: usbControllerText[usbControllerCodes[v.expected]] ?? v.expected }) : null];
    case 30: return [stage, t('timeline.35', { value: v.expected ?? 0 })];
    // Stage 32's cause is the native detach result, not a Win32 error or the reason table.
    // A failed request does not establish whether the kernel port remains or the PC reclaimed the device.
    case 32: return [stage, v.cause === 1 ? t('timeline.36') : v.cause === 2 ? t('timeline.37') : t('timeline.39', { isInteger: Number.isInteger(v.cause) ? t('timeline.38', { cause: v.cause }) : '' }), t('timeline.40')];
    case 24: return [stage, usbErrnoLabel(v.error), request];
    case 25: return [stage, v.error === 1 ? t('timeline.41') : t('timeline.42'), request];
    case 26: return [stage, t('timeline.43', { error: v.error }), request];
    case 27: return [stage, t('timeline.44', { value: v.items ?? 0 }), t('timeline.45', { value: v.kib ?? 0 }), t('timeline.29', { value: v.cause ?? 0 }), t('timeline.46', { value: v.expected ?? 0 })];
    case 28: return [stage, v.error ? t('timeline.47', { error: v.error }) : t('timeline.48'), t('timeline.49', { expected: v.expected })];
    case 17: return [stage, valueText('error', v.error), request];
    default: return [stage, v.error ? (own(clientCodes, reason) ?? reason ?? valueText('error', v.error)) : null, v.expected && ![17, 24, 25, 26, 28].includes(v.stage) ? valueText('expected', v.expected) : null];
  }
}
// A client usb event: the stage, then the counts and the first non-OK request in words.
export function usbClientDetail(values) {
  const v = values ?? {};
  return [clientUsbText[v.stage] ?? t('timeline.20', { stage: v.stage }), v.usbReq > 0 ? t('timeline.50', { usbRequestClassText: usbRequestClassText(v.usbReq), value: v.error ? `(${usbErrnoLabel(v.error)})` : '' }) : null, v.stage === 10 && Object.hasOwn(v, 'usbBack') ? clientUsbBack[v.usbBack] ?? null : null,
    ...['usbDevices', 'usbUrbs', 'usbKib', 'usbStalls', 'usbFailed', 'usbTimeouts', 'usbSlow', 'elapsedMs', 'usbClass'].filter(key => Number.isFinite(v[key])).map(key => valueText(key, v[key]))].filter(Boolean);
}
const valueText = (key, value) => `${own(valueNames, key) ?? key} ${typeof value === 'boolean' ? (value ? t('common.115') : t('common.116')) : ['error', 'action', 'stage', 'width', 'height'].includes(key) ? String(value) : ['current', 'latest'].includes(key) ? `${Math.floor(value / 1e6)}.${Math.floor(value / 1000) % 1000}.${value % 1000}` : integer(value)}`;
function statsText(v) {
  const f = key => Number.isFinite(v[key]) ? v[key] : null;
  return [f('textureFps10') ?? f('decodeFps10'), f('rttMs'), f('inputFrameP50Us')].some(x => x !== null)
    ? [((f('textureFps10') ?? f('decodeFps10')) !== null ? t('timeline.51', { fixed: fixed((f('textureFps10') ?? f('decodeFps10')) / 10) }) : null), f('rttMs') !== null ? t('timeline.52', { integer: integer(f('rttMs')) }) : null, f('inputFrameP50Us') !== null ? t('timeline.53', { ms: ms(f('inputFrameP50Us')) }) : null].filter(Boolean) : [];
}
export function clientEntry(row, index = 0) {
  const values = row.values ?? {};
  return { key: `client:${row.runId ?? ''}:${row.sequence ?? ''}:${index}:${row.time}`, time: Date.parse(row.time), receivedAt: row.receivedAt, source: 'client', event: row.event, title: (own(clientEvents, row.event) ?? row.event) + (row.mode && !['streamStats'].includes(row.event) && !/^stream/.test(row.event) ? ` · ${t('common.67')}` : ''),
    detail: row.event === 'streamStats' ? statsText(values) : row.event === 'usb' ? usbClientDetail(values) : Object.entries(values).filter(([key]) => !['samples', 'windowMs'].includes(key)).map(([key, value]) => valueText(key, value)),
    // `ok` is the verdict; a code beside ok:true is information (a record without ok keeps its code as the failure).
    failure: values.ok === false || (!!row.code && values.ok !== true), code: row.code ?? null, codeLabel: row.code ? own(clientCodes, row.code) ?? null : null, desktopId: row.desktopId ?? null, account: row.account ?? null, version: row.version ?? null,
    ids: ['sessionId', 'operationId', 'requestId'].filter(key => row[key]).map(key => [key, row[key]]), stats: row.event === 'streamStats' };
}
// action254 is a protected numeric-only record of an actual encoded output.
// An initialized device/selected setting (or an old live snapshot) cannot
// become a successful GPU-use label. The timestamp is historical, not liveness.
const gpuFailures = { 32: t('timeline.54'), 33: t('timeline.55'), 34: t('timeline.56'), 35: t('timeline.57'), 36: t('timeline.58'), 37: t('timeline.59'), 38: t('timeline.60'), 39: t('timeline.61'), 40: t('timeline.62'), 41: t('timeline.63'), 42: t('timeline.64'), 43: t('timeline.65'), 44: t('timeline.66') };
function encoderHostUsage(v) {
  const names = { 1: 'CPU x264', 2: 'CPU Windows MFT', 3: 'NVIDIA NVENC', 4: 'AMD AMF', 5: 'Intel QSV' };
  const vendors = { 1: 0, 2: 0, 3: 0x10de, 4: 0x1002, 5: 0x8086 };
  const known = Number.isInteger(v.stage) && Object.hasOwn(names, v.stage), hardware = known && v.stage >= 3;
  const codes = Number.isInteger(v.fallback) && (v.fallback === 0 || (v.fallback >= 32 && v.fallback <= 44)) && Number.isInteger(v.error) && v.error >= 0 && v.error <= 7;
  const verified = known && codes && v.ok === true && v.active === true && v.format === vendors[v.stage] && v.owned === hardware;
  const cpuFailures = { 1: t('timeline.67'), 2: t('timeline.68'), 3: t('timeline.69'), 4: t('timeline.70'), 5: t('timeline.71'), 6: t('timeline.72'), 7: t('timeline.73') };
  return {
    title: verified ? t('timeline.74', { value: names[v.stage] }) : t('timeline.76', { value: known ? names[v.stage] : t('timeline.75') }),
    detail: [verified ? (hardware ? t('timeline.77') : t('timeline.78')) : t('timeline.79'),
      [0, 1].includes(v.items) ? t('timeline.80', { value: v.items + 1 }) : null,
      Number.isInteger(v.fallback) ? gpuFailures[v.fallback] ?? null : null, Number.isInteger(v.error) ? cpuFailures[v.error] ?? null : null, t('timeline.81')].filter(Boolean),
  };
}
// service.hpp recordRenderObservation: these fields are adapter identities.
// `ok` says whether the compositor used a known hardware vendor; it does not
// report a failed operation or the encoder that actually produced video.
function renderGpuDetail(v) {
  const adapter = (vendor, device) => {
    if (vendor === 0) return t('timeline.82');
    if (vendor === 1) return t('timeline.83');
    if (!Number.isInteger(vendor) || vendor < 0 || vendor > 0xffff) return t('common.17');
    const names = {0x10de: 'NVIDIA', 0x1002: 'AMD', 0x8086: 'Intel', 0x1414: 'Microsoft'};
    const label = names[vendor] ?? t('timeline.84', { toString: vendor.toString(16).toUpperCase().padStart(4, '0') });
    return label + (Number.isInteger(device) && device > 0 && device <= 0xffff ? t('timeline.85', { toString: device.toString(16).toUpperCase().padStart(4, '0') }) : '');
  };
  return [t('timeline.86', { adapter: adapter(v.stage, v.expected) }),
    t('timeline.87', { adapter: adapter(v.error, v.missing) }),
    v.ok === true ? t('timeline.88') : t('timeline.89'),
    t('timeline.90')];
}
// Two producers share action280. Require the complete producer field set;
// a stage number or absent thumbnail size cannot identify a display trace.
function displayOrThumbnail(v) {
  const exact = names => Object.keys(v).length === names.length && names.every(key => Object.hasOwn(v, key));
  const bit = key => typeof v[key] === 'boolean';
  const uint = (key, max = 0xffffffff) => Number.isInteger(v[key]) && v[key] >= 0 && v[key] <= max;
  const raw = () => t('timeline.91') + Object.entries(v).filter(([, value]) => typeof value === 'boolean' || Number.isFinite(value)).map(([key, value]) => `${key}=${value}`).join(' ');
  if (exact(['action','stage','ok','kib']) && uint('stage',5) && uint('kib') && bit('ok') && v.ok === (v.stage !== 0)) {
    return {title:t('timeline.92'),detail:[hostThumbnailText[v.stage],v.stage === 1 ? valueText('kib',v.kib) : null],failure:v.stage === 0};
  }
  if (exact(['action','stage','ok','cause']) && v.stage === 5 && v.ok === false && v.cause === 1) {
    return {title:t('timeline.93'),detail:[t('timeline.94'),t('timeline.95'),raw()],failure:false};
  }
  if (exact(['action','stage','active','ok','format','cause','error','owned','pending']) && v.stage === 3 &&
      ['active','ok','owned','pending'].every(bit) && uint('format',3) && uint('cause',9) && uint('error') &&
      (v.owned || !v.pending) && (v.active ? v.ok === (v.format !== 3) : !v.ok && v.format === 0 && v.cause === 0 && v.error === 0)) {
    const complete = v.active && v.ok && v.format === 1 && v.cause === 0 && v.error === 0 && v.owned && !v.pending;
    const result = !v.active ? t('timeline.96') : v.format === 0 ? t('timeline.97')
      : v.format === 1 ? complete ? t('timeline.98') : t('timeline.99')
      : v.format === 2 ? t('timeline.100') : t('timeline.101');
    // owned is statePreserved.has_value(); pending is its value: state-file
    // evidence, never device ownership or an operation waiting in a queue.
    const state = !v.owned ? t('timeline.102') : v.pending ? t('timeline.103') : t('timeline.104');
    return {title:t('timeline.93'),detail:[result,state,v.cause ? t('timeline.105', { cause: v.cause }) : null,v.error ? t('timeline.106', { error: v.error }) : null,t('timeline.107'),raw()],failure:v.active && v.format === 3};
  }
  if (exact(['action','stage','items','monitors','owned','ok','active','changed','width','height','expected','missing','recovery','stale']) &&
      [1,2,4].includes(v.stage) && ['owned','ok','active','changed','recovery','stale'].every(bit) && uint('items',2) && uint('monitors',2) &&
      uint('width',32768) && uint('height',32768) && uint('expected') && uint('missing')) {
    return {title:t('timeline.108'),detail:[{1:t('timeline.109'),2:t('timeline.110'),4:t('timeline.111')}[v.stage],
      v.owned ? t('timeline.112', { monitors: v.monitors }) : t('timeline.113'),v.items < 2 ? t('timeline.80', { value: v.items + 1 }) : null,
      v.active ? t('timeline.114') : t('timeline.115'),v.ok ? `${v.width}×${v.height}` : t('timeline.116'),
      v.recovery ? t('timeline.117') : null,v.stale ? t('timeline.118') : null,t('timeline.119'),raw()],failure:false};
  }
  return {title:t('timeline.120'),detail:[t('timeline.121'),raw()],failure:false};
}
// Legacy worker.stream.failure also carried healthy capture observations.
// Decode only the released four-field shape; never change its original values.
// hostAction 200 stage 243 reason (error & 255; apps/windows_host/src/gpu_capture.hpp GpuPathReason). 5-8 are the
// GPU path breaking (the Host records them ok:false); the others say why a capture took the CPU path.
const gpuPathReasons = {
  1: t('timeline.122'),
  2: t('timeline.123'),
  3: t('timeline.124'),
  4: t('timeline.125'),
  5: t('timeline.126'),
  6: t('timeline.127'),
  7: t('timeline.128'),
  8: t('timeline.129'),
  9: t('timeline.130'),
};
// hostAction 200 stage 244 (apps/windows_host/src/secure_desktop_notice.hpp): error & 255, and the seconds of a return.
const secureDesktopNotice = {
  1: t('timeline.131'),
  2: t('timeline.132'),
  3: t('timeline.133'),
  4: t('timeline.134'),
  // 5-8 (the SYSTEM helper that streams the real prompt): the stream is kept in every case.
  5: t('timeline.135'),
  6: t('timeline.136'),
  7: t('timeline.137'),
  8: t('timeline.138'),
};
// hostAction 200 stage 245, the way back to the GPU encoder after a missed output deadline:
// error = kind | attempt << 8 | GPU code << 16. The stream runs on the CPU encoder meanwhile, so none is a failed stream.
const gpuEncoderReturn = {
  1: t('timeline.139'),
  2: t('timeline.140'),
  3: t('timeline.141'),
  4: t('timeline.142'),
  5: t('timeline.143'),
};
export function streamGpuObservation(v) {
  if (!v || v.action !== 200 || ![242, 243, 244, 245].includes(v.stage)) return null;
  const uint = n => Number.isInteger(n) && n >= 0 && n <= 0xffffffff;
  const keys = ['action', 'stage', 'error', 'ok'];
  const raw = t('timeline.91') + keys.filter(key => uint(v[key]) || typeof v[key] === 'boolean').map(key => `${key}=${v[key]}`).join('; ');
  const title = v.stage === 242 ? t('timeline.144') : v.stage === 243 ? t('timeline.145') : v.stage === 245 ? t('timeline.146') : t('timeline.147');
  const historical = v.stage === 244 ? t('timeline.148') : v.stage === 245 ? t('timeline.149') : t('timeline.150');
  const unknown = () => ({ title: title + t('timeline.151'), detail: [t('timeline.152'), historical, raw], failure: v.ok === false });
  if (Object.keys(v).length !== keys.length || !keys.every(key => Object.hasOwn(v, key)) || typeof v.ok !== 'boolean' || !uint(v.error)) return unknown();
  if (v.stage === 243) {
    const reason = v.error & 0xff, extra = v.error >>> 8, broken = reason >= 5 && reason <= 8;
    // Only a broken path carries its failed step and result in the upper bits.
    if (!own(gpuPathReasons, reason) || (extra && !broken)) return unknown();
    return { title, detail: [own(gpuPathReasons, reason), broken && extra ? t('timeline.153', { value: extra & 0xff, value2: extra >>> 8 }) : null, historical, raw].filter(Boolean), failure: broken };
  }
  if (v.stage === 245) {
    const kind = v.error & 0xff, attempt = (v.error >>> 8) & 0xff, gpu = v.error >>> 16;
    if (!gpuEncoderReturn[kind]) return unknown();
    // The GPU code is named where the table knows it; otherwise it stays in the raw line only.
    return { title, detail: [gpuEncoderReturn[kind], attempt ? t('timeline.154', { attempt }) : null, gpu ? gpuFailures[gpu] ?? null : null, historical, raw].filter(Boolean), failure: false };
  }
  if (v.stage === 244) {
    const what = v.error & 0xff, seconds = v.error >>> 8;
    if (!own(secureDesktopNotice, what) || (seconds && what !== 2)) return unknown();
    // A lock or the limit ends the stream as decided (Console or a restart), not a product failure.
    return { title, detail: [own(secureDesktopNotice, what), what === 2 ? t('timeline.155', { seconds }) : null, historical, raw].filter(Boolean), failure: false };
  }
  // stream_gpu_priority.hpp: nibbles wanted/applied; bits8..12 HAGS known,
  // HAGS on, privilege, GPU thread raised, previous class already sufficient.
  if (v.error > 0x1fff) return unknown();
  const wanted = v.error & 15, applied = (v.error >>> 4) & 15;
  const hagsKnown = !!(v.error & 0x100), hagsOn = !!(v.error & 0x200), threadRaised = !!(v.error & 0x800), already = !!(v.error & 0x1000);
  if (![4, 5].includes(wanted) || ![4, 5, 15].includes(applied) || (hagsOn && !hagsKnown) ||
      wanted !== (hagsKnown && !hagsOn ? 5 : 4) || (already && applied !== 15) || (applied !== 15 && applied > wanted)) return unknown();
  const names = { 4: t('timeline.156'), 5: t('timeline.157') }, confirmed = already || applied !== 15;
  return { title, detail: [
    t('timeline.159', { value: names[wanted], value2: already ? t('timeline.158') : applied === 15 ? t('common.119') : names[applied] }),
    t('timeline.160', { value: hagsKnown ? hagsOn ? t('common.118') : t('common.96') : t('common.119'), value2: threadRaised ? t('common.21') : t('common.64') }),
    historical, raw,
  ], failure: !confirmed };
}
// hostAction 282 (apps/windows_host/src/native_render_adapter.hpp): the render adapter re-arm after a capture on WARP.
const renderRearmSkips = {
  1: t('timeline.161'), 2: t('timeline.162'), 3: t('timeline.163'),
  4: t('timeline.164'), 5: t('timeline.165'), 6: t('timeline.166'),
  7: t('timeline.167'), 8: t('timeline.168'),
};
export function renderRearmDetail(v) {
  if (v.stage === 1) return [t('timeline.169')];
  if (v.stage === 2) return [v.ok === false ? t('timeline.170') : t('timeline.171')];
  if (v.stage === 3) return [t('timeline.172'), renderRearmSkips[v.error] ?? valueText('error', v.error)];
  return [t('timeline.20', { stage: v.stage })];
}
// A Console park (hostAction 215, labels.js hostConsoleParkText): the Host stopped its own virtual outputs so that the
// Proxmox console shows Windows, and why no worker could do it instead.
export function consoleParkDetail(v) {
  if (v.stage === 2) return [hostConsoleParkText[2]];
  if (v.stage !== 1) return [t('timeline.20', { stage: v.stage })];
  const why = hostConsoleParkCauseText[v.cause] ?? (v.cause ? valueText('cause', v.cause) : null);
  if (v.ok) return [hostConsoleParkText[1], why];
  // Refused before the stop, or the stop itself failed (changed: it was tried).
  const refusal = v.error === 1 && v.changed ? t('timeline.173') : hostConsoleParkRefusalText[v.error] ?? (v.error ? valueText('error', v.error) : null);
  return [t('timeline.174'), refusal, why];
}
// Input counts and levels (hostAction 200 stage 246, labels.js hostInputKindText): never an input itself.
export function inputRecordObservation(v) {
  if (!v || v.action !== 200 || v.stage !== 246 || !Number.isInteger(v.error) || v.error < 0) return null;
  const kind = v.error & 255, value = v.error >>> 8, label = hostInputKindText[kind] ?? t('timeline.175', { kind });
  const level = code => own(hostInputLevelText, code) ?? t('timeline.176', { code });
  const detail = [6, 16].includes(kind) ? [label, t('common.126', { value })]
    : kind === 10 ? [label, t('timeline.177', { level: level(value & 15), level2: level((value >> 4) & 15) })]
    : [18, 19].includes(kind) ? [label, hostInputInjectorReasonText[(value >> 4) & 15] ?? null, t('timeline.178', { level: level(value & 15) })]
    : [label, t('timeline.179', { integer: integer(value) })];
  return { title: t('timeline.180'), detail: detail.filter(Boolean), failure: false };
}
// Host settings' finish of an install (hostAction 276, labels.js hostFinishStepText): one record per step.
export function finishStepDetail(v) {
  const step = hostFinishStepText[v.stage];
  if (!step) return [t('timeline.20', { stage: v.stage })];
  const code = (v.stage === 1 ? hostFinishDisplayCodes : hostFinishWinfspCodes)[v.error];
  const reason = v.error ? own(hostFinishCodeText, code) ?? valueText('error', v.error) : null;
  const restart = v.reboot ? t('timeline.10') : null;
  if (v.stage === 2) return [step, v.ok ? (v.installed ? t('timeline.181') : reason ?? t('timeline.182')) : t('timeline.183'), v.ok ? null : reason, restart];
  return [step, v.ok ? (v.installed ? (v.started ? t('timeline.184') : t('timeline.185')) : t('timeline.182')) : t('timeline.183'), reason,
    v.changed ? t('timeline.186') : null, restart, v.restartAttempted ? t('timeline.187') : null,
    v.running ? t('timeline.188') : v.restartAttempted ? t('timeline.189') : null, v.recovery ? t('timeline.190') : null];
}
// What removing the Host cleaned up (hostAction 285, labels.js hostRemovalReasons). Read after a reinstall: the record
// stays in the Host's journal on that machine.
export function removalDetail(v) {
  const reason = hostRemovalReasonText[hostRemovalReasons[v.error]] ?? (v.error ? valueText('error', v.error) : null);
  if (reason) return [reason];
  const certificate = hostRemovalCertificateText[hostRemovalCertificates[v.cause]] ?? (Number.isInteger(v.cause) ? valueText('cause', v.cause) : null);
  return [v.ok ? t('timeline.191') : t('timeline.192'), t('timeline.193', { value: v.items ?? 0 }), v.missing ? t('timeline.194', { missing: v.missing }) : null, v.pending ? t('timeline.195') : null,
    t('timeline.196', { value: v.dropped ?? 0 }), v.expected ? t('timeline.197', { expected: v.expected }) : null, certificate, v.owned ? t('timeline.198') : t('timeline.199')];
}
export function hostEntries(hostList) {
  const out = [];
  for (const host of hostList ?? []) for (const [index, row] of (host.diagnostics?.events ?? []).entries()) {
    // hostAction carries a fixed action code: name it in the title.
    const v = row.values ?? {};
    const renderObservation = row.event === 'hostAction' && v.action === 211;
    const gpuObservation = row.event === 'hostAction' ? streamGpuObservation(v) ?? inputRecordObservation(v) : null;
    const encoderUsage = row.event === 'hostAction' && v.action === 254 ? encoderHostUsage(v) : null;
    const displayTrace = row.event === 'hostAction' && v.action === 280 ? displayOrThumbnail(v) : null;
    // Hosts 0.2.73-0.2.76 sent print records as 240 too; those carry `ok`.
    const print = v.action === 242 || (v.action === 240 && Object.hasOwn(v, 'ok'));
    const action = row.event === 'hostAction' ? hostActions[print ? 242 : v.action] : null;
    // A remote Host update names its state and reason instead of index numbers.
    const update = gpuObservation ? gpuObservation.detail : renderObservation ? renderGpuDetail(v) : encoderUsage ? encoderUsage.detail : v.action === 230 ? [hostUpdateStateText[hostUpdateEventStates[v.stage - 1]], hostUpdateCodeText[hostUpdateEventCodes[v.error]]]
      : print ? [hostPrintText[v.stage] ?? t('timeline.20', { stage: v.stage }), v.error ? (v.stage === 30 ? hostPrintDefaultReasonText[v.error] : null) ?? valueText('error', v.error) : null, v.expected ? valueText('expected', v.expected) : null, v.cause ? t('timeline.200', { cause: v.cause }) : null]
      // Streaming drive sharing (hostAction 247): the stage, the reason and, for a failed mapping, the Windows error.
      : v.action === 250 ? usbHostDetail(v)
      : v.action === 215 ? consoleParkDetail(v)
      : v.action === 276 ? finishStepDetail(v)
      : v.action === 285 ? removalDetail(v)
      : v.action === 282 ? renderRearmDetail(v)
      : v.action === 291 ? [v.active ? t('timeline.201') : t('timeline.202')]
      : v.action === 273 ? displayInventoryDetail(v)
      : v.action === 274 ? bootReadyDetail(v)
      : v.action === 247 ? [hostDriveText[v.stage] ?? t('timeline.20', { stage: v.stage }), v.error ? hostDriveErrorText[v.error] ?? valueText('error', v.error) : null, v.expected ? valueText('expected', v.expected) : null, v.missing ? valueText('missing', v.missing) : null, v.cause ? t('timeline.200', { cause: v.cause }) : null]
      // A media backlog the Host degraded, and a broker channel the Host closed (with its Win32 error).
      : v.action === 240 ? [hostMediaDropText[v.stage] ?? t('timeline.20', { stage: v.stage }), Number.isFinite(v.dropped) ? valueText('dropped', v.dropped) : null]
      // A clipboard transfer on the desktop (hostAction 253): which side, what, how big, and why it failed.
      : v.action === 253 && v.stage >= 10 ? [hostClipboardStageText[v.stage] ?? t('timeline.20', { stage: v.stage }), v.error ? (hostClipboardNoteText[v.error - 1] ?? valueText('error', v.error)) : null,
        v.items ? t('timeline.203', { filter: hostClipboardFormatBits.filter(([bit]) => v.items & bit).map(([, name]) => name).join(', ') || t('common.121') }) : null]
      : v.action === 253 ? [hostClipboardStageText[v.stage] ?? t('timeline.20', { stage: v.stage }), hostClipboardFormatText[v.format] ?? null, v.items ? valueText('items', v.items) : null, v.format && Number.isFinite(v.kib) ? valueText('kib', v.kib) : null,
        v.error ? (clientCodes[hostClipboardReasons[v.error - 1]] ?? hostClipboardReasons[v.error - 1] ?? valueText('error', v.error)) : null, v.cause ? t('timeline.204', { value: hostClipboardCauseText[hostClipboardCauses[v.cause - 1]] ?? clientCodes[hostClipboardCauses[v.cause - 1]] ?? hostClipboardCauses[v.cause - 1] ?? v.cause }) : null, hostClipboardWin32Text(v.win32)]
      : v.action === 241 ? [hostChannelCloseText[hostChannelCloseCodes[v.stage]] ?? t('timeline.20', { stage: v.stage }), v.error ? valueText('error', v.error) : null]
      // The UDP relay leg: whether it bound (the relay's list index) and which path carries video, with the reason it changed.
      : v.action === 270 ? [hostUdpProbeText[v.stage] ?? t('timeline.20', { stage: v.stage }), Number.isInteger(v.error) ? t('timeline.205', { value: v.error + 1 }) : null]
      : v.action === 271 ? [hostUdpPathText[v.stage] ?? t('timeline.20', { stage: v.stage }), hostUdpPathReasonText[v.error] ?? (v.error ? valueText('error', v.error) : null)]
      // A launcher picture (hostAction 280): what the Host answered and the picture's size, never the picture.
      : displayTrace ? displayTrace.detail : null;
    // 282 waits and skips are recorded ok:false; only an unorderly re-arm or a re-arm that cannot happen is a problem.
    const rearmFailure = row.event === 'hostAction' && v.action === 282 ? (v.stage === 2 ? v.ok === false : v.stage === 3 && [2, 3, 7].includes(v.error)) : null;
    const rawDiagnostic = (gpuObservation ?? displayTrace)?.detail.find(value => typeof value === 'string' && value.startsWith(t('timeline.91')));
    const detail = update ? update.filter(value => value && value !== rawDiagnostic) : Object.entries(row.values ?? {}).filter(([key]) => !(action && key === 'action')).map(([key, value]) => valueText(key, value));
    out.push({ key: `host:${host.desktopId}:${index}:${row.time}`, time: Date.parse(row.time), source: 'host', event: row.event, title: gpuObservation?.title ?? displayTrace?.title ?? encoderUsage?.title ?? (action ? t('timeline.206', { action }) : own(hostEvents, row.event) ?? row.event), detail, ...(rawDiagnostic ? {rawDiagnostic} : {}), failure: gpuObservation ? gpuObservation.failure : displayTrace ? displayTrace.failure : rearmFailure !== null ? rearmFailure : !renderObservation && row.values?.ok === false, desktopId: host.desktopId, account: null, version: row.version ?? host.diagnostics.version });
  }
  return out;
}
// One VM's Host events for its own timeline: the broker's rolling history (its
// diagnostics view) once it holds any, else the latest report in the Host list.
export function desktopHostList(ctx, desktopId) {
  const history = ctx.value('diagnostics:' + desktopId)?.history?.events;
  return Array.isArray(history) && history.length ? [{ desktopId, diagnostics: { version: null, events: history } }] : (ctx.value('hosts')?.hosts ?? []).filter(row => row.desktopId === desktopId);
}
// All entries newest first. `clientRows` may be the global or a per-VM list.
export function collect(ctx, { clientRows = ctx.value('clientDiagnostics')?.events ?? [], hostList = ctx.value('hosts')?.hosts ?? [] } = {}) {
  const { users, desktops, audit } = ctx.state.data;
  const usersById = new Map(users.map(u => [u.id, u])), desktopsById = new Map(desktops.map(d => [d.id, d]));
  return [...(audit ?? []).map(e => auditEntry(e, usersById, desktopsById)), ...clientRows.map(clientEntry), ...hostEntries(hostList)]
    .filter(entry => Number.isFinite(entry.time)).sort((a, b) => b.time - a.time);
}

const sources = { audit: t('common.85'), client: t('common.128'), host: 'Host' };
const idNames = { sessionId: t('timeline.211'), operationId: t('timeline.212'), requestId: t('timeline.213') };
// What a failed connection means for the operator (labels.js failureAdvice):
// a client failure or a failed login. Host records keep their own wording. A
// code without advice is named by its label (or 원인 미확인) and never suggests a fix.
export function failureAdviceFor(entry) {
  if (!entry.failure) return null;
  if (entry.source === 'audit') return entry.event === 'login_failed' ? failureAdvice.login_failed : null;
  if (entry.source !== 'client') return null;
  const known = entry.code ? failureAdvice[entry.code] : null;
  if (known) return known;
  const named = entry.codeLabel && entry.code !== 'unknown_failure';
  return { who: 'unknown', what: named ? entry.codeLabel : t('labels.108'), fix: null };
}
const whoOf = entry => entry.source === 'audit' ? entry.actor : entry.account;
const subjectOf = entry => entry.source === 'audit' ? entry.account ?? entry.actor : entry.account;
const person = (ctx, name) => name && ctx.userByName?.(name) ? ctx.link(name, 'user', name) : name || '—';
const desktopOf = (ctx, entry) => entry.desktopId ? ctx.desktopById(entry.desktopId) : null;
const place = (ctx, entry) => { const desktop = desktopOf(ctx, entry); return desktop ? ctx.link(desktop.name, 'desktop', desktop.id) : entry.desktopId ?? '—'; };
const result = entry => entry.failure ? el('span', { class: 'is-bad' }, t('common.110')) : t('common.57');
const when = entry => el('time', { datetime: new Date(entry.time).toISOString(), title: date(entry.time) }, clock(entry.time));
// 원인 of a failure in a few words (조치 필요 under it when the administrator has something to do); for a record
// that did not fail, what it says (내용).
export function causeCell(entry) {
  const advice = failureAdviceFor(entry);
  if (advice) return [advice.what, advice.who === 'admin' ? el('small', { class: 'log-todo' }, t('timeline.needsAction')) : null];
  if (entry.failure) return entry.codeLabel ?? (entry.detail.join(' · ') || t('labels.108'));
  return entry.detail.join(' · ') || '—';
}
// The whole record as a form table: the row's detail (the window of 기록, or the line opened under the row in a
// desktop's or a user's window). What to do about a failure (조치) is read here, not in the row.
export function entryFacts(ctx, entry) {
  const advice = failureAdviceFor(entry), who = whoOf(entry);
  const ids = (entry.ids ?? []).map(([key, id]) => `${own(idNames, key) ?? key} ${id}`).join(' · ');
  return facts([
    [t('timeline.colTime'), `${day(entry.time)} ${clock(entry.time)}`],
    [t('monitor.12'), sources[entry.source] ?? entry.source],
    [t('timeline.colEvent'), entry.title],
    who ? [entry.source === 'audit' ? t('timeline.actor') : t('timeline.colUser'), person(ctx, who)] : null,
    entry.source === 'audit' && entry.account ? [t('timeline.subject'), person(ctx, entry.account)] : null,
    entry.desktopId ? [t('timeline.colDesktop'), place(ctx, entry)] : null,
    [t('timeline.colResult'), result(entry)],
    entry.failure ? [t('timeline.colCause'), advice?.what ?? entry.codeLabel ?? t('labels.108')] : null,
    advice?.fix ? [t('timeline.fix'), advice.fix] : null,
    entry.code ? [t('timeline.code'), [entry.codeLabel && entry.codeLabel !== advice?.what ? entry.codeLabel + ' ' : null, el('code', {}, entry.code)]] : null,
    entry.detail.length ? [t('timeline.colDetail'), entry.detail.join(' · ')] : null,
    entry.version ? [t('system.21'), `${sources[entry.source] ?? ''} ${entry.version}`.trim()] : null,
    ids ? [t('timeline.ids'), ids] : null,
    entry.rawDiagnostic ? [t('common.120'), el('code', { class: 'log-raw' }, entry.rawDiagnostic)] : null,
  ], 'form-table log-facts');
}
// The window of one record on 기록 (dom.js detailWindow); 데스크톱 열기 when the record is about one.
export function logWindow(ctx, entry, close) {
  const desktop = desktopOf(ctx, entry);
  return detailWindow(ctx, { key: 'log:' + entry.key, kind: t('common.2'), title: entry.title, size: 'narrow', className: 'log-window', focusKey: 'log-open:' + entry.key, close,
    badges: entry.failure ? stateDot(t('common.110'), 'red') : null,
    tabs: [{ key: 'entry', label: t('common.2'), nodes: [entryFacts(ctx, entry)] }],
    actions: desktop ? [ctx.link(t('common.136'), 'desktop', desktop.id, { class: 'button secondary' })] : [] });
}
// Records as a board, newest first, a heading row per day: 시간 / 사용자 / 데스크톱 / 작업 / 결과 / 원인 (or 내용).
// A row opens its detail: `open(entry)` (a window, 기록), or with `inline` (a name for this board) a line under the
// row (inside a window, where no second window can open).
export function logBoard(ctx, entries, { limit = 150, showVm = true, showAccount = true, cause = true, emptyTitle = t('timeline.217'), emptyText = '', open = null, inline = null } = {}) {
  if (!entries.length) return emptyState(emptyTitle, emptyText, { iconName: 'list', compact: true });
  const ui = ctx.state.ui, shown = entries.slice(0, limit);
  const columns = [{ label: t('timeline.colTime'), cls: 'col-time' }, showAccount ? { label: t('timeline.colUser'), cls: 'col-who' } : null, showVm ? { label: t('timeline.colDesktop'), cls: 'col-desktop is-left' } : null,
    { label: t('timeline.colEvent'), cls: 'col-event is-name' }, { label: t('timeline.colResult'), cls: 'col-result is-state' }, { label: cause ? t('timeline.colCause') : t('timeline.colDetail'), cls: 'col-cause is-left' }, { label: t('common.85'), cls: 'col-manage' }].filter(Boolean);
  const rows = [];
  let label = null;
  for (const entry of shown) {
    const today = day(entry.time);
    if (today !== label) { label = today; rows.push({ group: today, cls: 'log-day' }); }
    const key = inline ? `${inline}|${entry.key}` : entry.key, opened = inline && ui.logRow === key;
    const show = () => { if (inline) { ui.logRow = opened ? null : key; ctx.render({ refocus: { key: 'log-open:' + key } }); } else open?.(entry); };
    const advice = failureAdviceFor(entry);
    rows.push({ attrs: { class: 'is-pick' + (entry.failure ? ' is-failure' : '') + (opened ? ' is-on' : ''), 'data-entry': entry.key, 'data-advice': advice?.who ?? null, onclick: event => { if (!event.target?.closest?.('a,button,input,label,summary')) show(); } },
      cells: [when(entry), ...(showAccount ? [el('span', { title: entry.source === 'audit' && entry.actor ? t('timeline.actor') + ' ' + entry.actor : null }, person(ctx, subjectOf(entry)))] : []), ...(showVm ? [place(ctx, entry)] : []), entry.title, result(entry), causeCell(entry),
        button(t('common.91'), show, 'secondary small', null, { 'aria-label': t('common.131', { title: entry.title }), 'aria-expanded': inline ? String(!!opened) : null, 'data-focus': 'log-open:' + key })] });
    if (opened) rows.push({ detail: entryFacts(ctx, entry) });
  }
  return [board(columns, rows, { className: 'log-board' }), entries.length > limit ? el('p', { class: 'field-hint log-limit' }, t('timeline.218', { limit })) : null];
}
