import { safeHostEncoderDiagnostic } from './encoder-diagnostics.mjs';
// Deliberately independent of capability/readiness validation. These are
// untrusted host observations for administrators, never authority to connect.
import { consoleReasons } from './console-preparation.mjs';
export const diagnosticsProtocol = 'host-diagnostics-v1';
const object = value => value && typeof value === 'object' && !Array.isArray(value);
const phases = new Set(['unknown', 'empty', 'starting', 'stable', 'changing', 'stopped', 'recovery']);
const reasons = new Set(['', 'no_interactive_session', 'interactive_console_required', 'administrator_required', 'reboot_required', 'viewport_probe_pending', 'viewport_probe_failed', 'viewport_probe_timeout', 'viewport_recovery_required', 'owned_virtual_display_not_ready', 'native_display_not_ready', 'native_display_provider_unavailable', 'native_display_snapshot_invalid', 'native_display_configuration_untrusted', 'native_display_topology_mismatch', 'native_display_change_pending', 'native_topology_not_ready', 'native_topology_recovery_required']);
const nativeFailures = new Set(['', 'runtime_recovery_required', 'native_stream_profile_recovery_required', 'transport_open_failed', 'controller_start_failed', 'heartbeat_failed', 'internal_failure']);
const activationReasons = new Set(['', 'interactive_console_required', 'native_display_snapshot_invalid', 'native_display_configuration_untrusted', 'native_display_topology_mismatch', 'native_topology_query_failed', 'native_topology_plan_invalid', 'native_topology_mode_invalid', 'native_topology_baseline_required', 'native_topology_validate_failed', 'native_topology_changed', 'native_topology_apply_failed', 'native_topology_not_applied', 'native_topology_failed', 'native_activation_observation_pending', 'native_activation_lease_changed', 'native_activation_worker_unavailable']);
const events = new Set(['installerStart', 'installerStage', 'installerResult', 'displayObserved', 'driverPreserved', 'driverInstall', 'displayConfigure', 'modeCatalog', 'serviceState', 'brokerState', 'hostAction']);
for (const reason of ['native_topology_foreign_changed', 'native_topology_target_unavailable', 'native_topology_source_changed', 'native_topology_transform_mismatch', 'native_topology_refresh_mismatch', 'native_topology_scanline_mismatch', 'native_topology_size_mismatch', 'native_topology_format_mismatch', 'native_topology_placement_mismatch']) activationReasons.add(reason);
// Display v2 restore detail: expected/missing targets, the
// console fallback code and the console state; adapters of a
// state from before a restart (stale) and whether one was mapped (remapped).
// hostAction 240 counts the media packets the Host's broker
// channel's send queue skipped (dropped).
// hostAction 253 is a clipboard transfer (apps/windows_host/src/clipboard_diagnostic.hpp): format, item count, KiB, internal cause.
// connect-timing-v1 Hosts: hostAction 200 stage 10 (first video) carries the
// start's stages in ms (apps/windows_host/src/connect_stages.hpp).
// boot-ready-v1: hostAction 274 (apps/windows_host/src/boot_timeline.hpp) is one service start's way to 연결 준비됨:
// Windows uptime when the service started (uptimeMs) and each stage in ms after that start.
export const bootStageFields = ['configMs', 'brokerMs', 'virtualDisplayMs', 'logonMs', 'sessionWorkerMs', 'readyMs'];
const logNumbers = new Set(['action', 'stage', 'runSeconds', 'monitors', 'width', 'height', 'error', 'expected', 'missing', 'missingOwned', 'fallback', 'stale', 'dropped', 'format', 'items', 'kib', 'cause', 'win32',
  'admitMs', 'workerMs', 'displayMs', 'mediaMs', 'frameMs', 'uptimeMs', ...bootStageFields]);
const logBooleans = new Set(['ok', 'installed', 'started', 'active', 'owned', 'pending', 'recovery', 'reboot', 'remote', 'running', 'registered', 'online', 'changed', 'restartAttempted', 'consoleExpected', 'consoleActive', 'remapped']);
const time = value => typeof value === 'string' && /^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$/.test(value) && Number.isFinite(Date.parse(value)) && new Date(value).toISOString() === value;
// Optional native profile-recovery explanation. Fixed
// vocabularies mirror apps/windows_host/src/native_recovery_trace.hpp; an
// unknown value drops the whole optional object, never the report.
export const recoveryCodes = Object.freeze({
  path: new Set(['none', 'expired_restore', 'retirement', 'expired_retirement', 'in_service']),
  blocker: new Set(['none', 'started', 'recovery_running', 'reservation_held', 'session_active', 'not_selected', 'retirement_archive_mismatch', 'profile_missing', 'launch_missing', 'records_invalid', 'profile_restored', 'launch_mismatch', 'launch_current_service', 'console_session_required', 'logon_unverified', 'sdk_not_quiescent', 'original_service_alive', 'original_worker_alive', 'retry_budget_exhausted']),
  quiescent: new Set(['none', 'ok', 'not_selected', 'controller_active', 'reservation_held', 'change_pending', 'instance_mismatch', 'records_missing', 'profile_changed', 'ledger_unresolved', 'transport_unavailable', 'sdk_present', 'failed']),
  ledger: new Set(['none', 'ok', 'launch_mismatch', 'profile_restored', 'snapshot_shape', 'runtime_missing', 'identity_missing', 'runtime_invalid', 'identity_invalid', 'runtime_instance', 'reclaim_present', 'settled_shape', 'settled_predecessor', 'pending_baseline', 'pending_scope', 'pending_identity', 'pending_outputs', 'pending_evidence', 'runtime_not_stable', 'settled_resize', 'launch_identity', 'launch_runtime', 'launch_viewport_conflict', 'viewport_missing', 'viewport_invalid', 'viewport_baseline', 'viewport_source', 'viewport_lease', 'viewport_outputs', 'viewport_pending_profile', 'viewport_committed_profile', 'viewport_connector', 'ledger_bytes', 'private_displays', 'failed']),
  retirement: new Set(['none', 'ok', 'not_selected', 'controller_active', 'reservation_conflict', 'change_pending', 'instance_mismatch', 'transport_unavailable', 'handoff_rejected', 'records_missing', 'records_invalid', 'profile_restored', 'launch_mismatch', 'runtime_mismatch', 'observation_missing', 'expired_console_proof', 'owned_output_active', 'logon_same', 'observation_stale', 'console_hardware', 'scope_incomplete', 'target_unavailable', 'console_not_unique', 'console_not_primary', 'primary_ambiguous', 'observation_invalid', 'marker_conflict', 'archive_failed', 'identity_rejected', 'original_process_alive', 'executor_not_running', 'observation_changed', 'previous_marker', 'owner_unlinked', 'leases_present', 'commit_failed', 'runtime_unverified', 'failed']),
  worker: new Set(['none', 'succeeded', 'launch_missing', 'launch_mismatch', 'original_alive', 'worker_unavailable', 'identity_unverified', 'reserve_rejected', 'restore_rejected', 'restore_authority_lost', 'restore_topology_changed', 'restore_history', 'restore_prepare', 'restore_validate', 'restore_apply', 'restore_not_restored', 'restore_complete', 'restore_unverified', 'observe_failed', 'retire_rejected', 'complete_rejected', 'reclaim_rejected', 'recover_unacknowledged', 'failed']),
  hold: new Set(['none', 'reservation', 'retirement_prepared', 'retirement_invalid', 'profile_not_restored', 'profile_without_launch', 'launch_profile_mismatch', 'launch_service_alive', 'launch_worker_alive', 'records_changed', 'records_unreadable', 'service_identity']),
  profilePhase: new Set(['none', 'prepared', 'active', 'restoring', 'restored', 'recovery', 'invalid']),
  runtimePhase: new Set(['none', 'stable', 'starting', 'changing', 'stopped', 'recovery', 'invalid']),
  launchService: new Set(['none', 'current', 'running', 'gone', 'unknown']),
  launchWorker: new Set(['none', 'current', 'running', 'gone', 'unknown']),
  logon: new Set(['none', 'same', 'new', 'unknown']),
  retirementPhase: new Set(['none', 'prepared', 'retired', 'completed', 'invalid']),
  expiredConsolePhase: new Set(['none', 'prepared', 'retired', 'completed', 'invalid']),
});
const recoveryRecords = ['profile', 'launch', 'runtime', 'identity', 'baseline', 'viewport', 'reclaim', 'retirement', 'expiredConsole'];
const recoveryFlags = ['profileResize', 'profileStopRequested', 'profileLaunchBound', 'runtimeCheckpoint', 'runtimeMatchesProfile', 'launchMatchesProfile', 'identityMatchesLaunch'];
export function safeRecoveryDiagnostic(value) {
  if (!object(value) || !object(value.records)) return null;
  const result = {};
  for (const [key, codes] of Object.entries(recoveryCodes)) { if (typeof value[key] !== 'string' || !codes.has(value[key])) return null; result[key] = value[key]; }
  result.records = {};
  for (const key of recoveryRecords) { if (!['absent', 'present', 'invalid'].includes(value.records[key])) return null; result.records[key] = value.records[key]; }
  for (const key of recoveryFlags) { if (value[key] !== null && typeof value[key] !== 'boolean') return null; result[key] = value[key]; }
  if (!Number.isSafeInteger(value.attempts) || value.attempts < 0 || value.attempts > 255 || typeof value.budgetExhausted !== 'boolean') return null;
  if (value.lastAttemptAgeMs !== null && (!Number.isSafeInteger(value.lastAttemptAgeMs) || value.lastAttemptAgeMs < 0 || value.lastAttemptAgeMs > 0xffffffff)) return null;
  result.attempts = value.attempts; result.budgetExhausted = value.budgetExhausted; result.lastAttemptAgeMs = value.lastAttemptAgeMs;
  return result;
}
function safeOwnedMonitors(display) {
  const rows=display.monitors, integer=(value,min,max)=>Number.isSafeInteger(value)&&value>=min&&value<=max;
  if(display.observed!==true||display.owned!==true||!Array.isArray(rows)||rows.length>2||rows.length!==display.monitorCount)return null;
  const ids=new Set(), result=[];let primaries=0;
  for(const row of rows){
    if(!object(row)||!['0','1'].includes(row.id)||ids.has(row.id)||
       !integer(row.width,1,16384)||!integer(row.height,1,16384)||
       !integer(row.left,-32768,32768)||!integer(row.top,-32768,32768)||
       typeof row.primary!=='boolean'||row.primary!==(row.left===0&&row.top===0)||
       (row.primary&&++primaries>1))return null;
    ids.add(row.id);result.push({id:row.id,width:row.width,height:row.height,left:row.left,top:row.top,primary:row.primary});
  }
  return result;
}
// Optional `devices` (schema 1), two optional groups re-encoded through fixed
// lists; anything off drops `devices` whole, never the report:
// - features (Host device_status.hpp): drives, usb, printers, audio - what
//   this VM can provide now for 공유 폴더, USB, 프린터 and 사운드카드;
// - inventory: displayAdapters and usbController - which
//   virtual display adapters and USB controller this VM holds.
const deviceCodes = { webClient: ['usable', 'disabled', 'missing', 'unknown'], controller: ['none', 'started', 'stopped', 'foreign', 'unknown'], reason: ['', 'print_spooler_unavailable', 'print_driver_unavailable', 'unknown'], spooler: ['running', 'stopped', 'starting', 'disabled', 'missing', 'unknown'], output: ['present', 'absent', 'remote', 'unknown'],
  blocker: ['', 'inventory_unavailable', 'current_not_ready', 'display_state_pending', 'busy'], cleanup: ['none', 'removed', 'pending_restart', 'failed', 'refused'], usbController: ['none', 'started', 'stopped', 'unmarked', 'duplicates', 'foreign', 'multiple', 'unknown'] };
const adapterCounts = ['current', 'legacy', 'duplicate', 'unproven', 'foreign'];
export function safeDeviceStatus(value) {
  if (!object(value) || value.schema !== 1) return null;
  const result = { schema: 1 }, code = (field, key) => { if (!deviceCodes[key].includes(field)) throw new Error('device_code'); return field; };
  try {
    if (['drives', 'usb', 'printers', 'audio'].some(key => value[key] !== undefined)) {
      const { drives, usb, printers, audio } = value;
      if (![drives, usb, printers, audio].every(object) || [drives.winfsp, drives.package, usb.supported, usb.package].some(bit => typeof bit !== 'boolean')) return null;
      Object.assign(result, { drives: { winfsp: drives.winfsp, package: drives.package, webClient: code(drives.webClient, 'webClient') }, usb: { supported: usb.supported, package: usb.package, controller: code(usb.controller, 'controller') },
        printers: { reason: code(printers.reason, 'reason'), spooler: code(printers.spooler, 'spooler') }, audio: { output: code(audio.output, 'output') } });
    }
    if (value.displayAdapters !== undefined) {
      const adapters = value.displayAdapters;
      if (!object(adapters) || typeof adapters.read !== 'boolean' || typeof adapters.currentStarted !== 'boolean' || adapterCounts.some(key => !Number.isSafeInteger(adapters[key]) || adapters[key] < 0 || adapters[key] > 64)) return null;
      result.displayAdapters = { read: adapters.read, ...Object.fromEntries(adapterCounts.map(key => [key, adapters[key]])), currentStarted: adapters.currentStarted, blocker: code(adapters.blocker, 'blocker'), cleanup: code(adapters.cleanup, 'cleanup') };
    }
    if (value.usbController !== undefined) result.usbController = code(value.usbController, 'usbController');
  } catch { return null; }
  return Object.keys(result).length > 1 ? result : null;
}
// One Host event as fixed fields: a known event name, its time and the listed
// numbers/booleans (anything else is dropped). Null when it is not an event.
export function safeHostEvent(row) {
  if (!object(row) || !time(row.time) || !events.has(row.event) || !object(row.values)) return null;
  const values = {};
  for (const [key, field] of Object.entries(row.values)) {
    if ((logBooleans.has(key) && typeof field === 'boolean') || (logNumbers.has(key) && Number.isSafeInteger(field) && field >= 0 && field <= 0xffffffff)) values[key] = field;
  }
  return { time: row.time, event: row.event, values };
}

// Rolling history of Host events: a report carries only the Host's newest few,
// so each accepted report's events are merged per Host, once each (same time,
// event and values), newest 500 for 7 days. Administrator evidence only.
export const hostEventHistoryLimit = 500, hostEventHistoryMs = 7 * 86400000;
const canonicalEvent = event => JSON.stringify({ time: event.time, event: event.event, values: Object.fromEntries(Object.entries(event.values).sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0))) });
export function recordHostEventHistory(store, hostId, report, now = Date.now()) {
  store.transaction(() => {
    const insert = store.db.prepare('INSERT INTO host_diagnostic_events(host_id,received_at,version,row) VALUES(?,?,?,?) ON CONFLICT(host_id,row) DO NOTHING');
    for (const event of report.events) insert.run(hostId, now, report.version, canonicalEvent(event));
    store.db.prepare('DELETE FROM host_diagnostic_events WHERE received_at<?').run(now - hostEventHistoryMs);
    store.db.prepare(`DELETE FROM host_diagnostic_events WHERE host_id=? AND id NOT IN (SELECT id FROM host_diagnostic_events WHERE host_id=? ORDER BY id DESC LIMIT ${hostEventHistoryLimit})`).run(hostId, hostId);
  });
}
// Newest first, each re-projected like a report's events.
export function hostEventHistory(store, hostId, now = Date.now()) {
  const events = [];
  for (const row of store.db.prepare(`SELECT received_at,version,row FROM host_diagnostic_events WHERE host_id=? AND received_at>=? ORDER BY id DESC LIMIT ${hostEventHistoryLimit}`).all(hostId, now - hostEventHistoryMs)) {
    let event = null; try { event = safeHostEvent(JSON.parse(row.row)); } catch { /* A damaged row is skipped. */ }
    if (event && /^\d{1,4}\.\d{1,4}\.\d{1,4}$/.test(row.version)) events.push({ ...event, version: row.version, receivedAt: new Date(row.received_at).toISOString() });
  }
  return events;
}
export function safeDiagnosticReport(value) {
  if (!object(value) || value.protocol !== diagnosticsProtocol || !['host-service', 'host-settings'].includes(value.source) || typeof value.version !== 'string' || !/^\d{1,4}\.\d{1,4}\.\d{1,4}$/.test(value.version) || !time(value.collectedAt) || !object(value.display) || !object(value.native) || !Array.isArray(value.events) || value.events.length > 8) return null;
  const result = { protocol: diagnosticsProtocol, source: value.source, version: value.version, collectedAt: value.collectedAt, display: {}, native: {}, events: [] };
  if(typeof value.display.observed !== 'boolean' || (value.display.observed ? !Number.isSafeInteger(value.display.observationAgeMs) || value.display.observationAgeMs < 0 || value.display.observationAgeMs > 0xffffffff : value.display.observationAgeMs !== null)) return null;
  result.display.observed=value.display.observed;result.display.observationAgeMs=value.display.observationAgeMs;
  for (const key of ['interactiveSession', 'consoleSession', 'captureAvailable']) {
    if (typeof value[key] !== 'boolean') return null;
    result[key] = value[key];
  }
  const console=value.consoleRecovery;
  if(object(console)){
    const booleans=['ready','eligible','complete','configuredIdentity','consoleIdentityKnown','consoleOccupied'],counts=['activeRdpSessions','disconnectedRdpSessions','disconnectedConsoleSessions','unknownSessions','lockedSessions'];
    if(booleans.every(k=>typeof console[k]==='boolean')&&counts.every(k=>Number.isSafeInteger(console[k])&&console[k]>=0&&console[k]<=64)){
      result.consoleRecovery={reason:console.reason===''||consoleReasons.has(console.reason)?console.reason:'windows_session_unknown'};
      for(const key of [...booleans,...counts])result.consoleRecovery[key]=console[key];
    }
  }
  for (const key of ['installed', 'deviceReady', 'owned', 'activeDisplay', 'configurationPending', 'recoveryRequired', 'rebootRequired', 'remoteSession']) {
    if (typeof value.display[key] !== 'boolean') return null;
    result.display[key] = value.display[key];
  }
  for (const key of ['monitorCount', 'width', 'height']) {
    const n = value.display[key];
    if (!Number.isSafeInteger(n) || n < 0 || n > (key === 'monitorCount' ? 2 : 16384)) return null;
    result.display[key] = n;
  }
  result.display.reason = reasons.has(value.display.reason) ? value.display.reason : 'viewport_probe_failed';
  const monitors=safeOwnedMonitors(value.display);
  if(monitors!==null)result.display.monitors=monitors;
  if (typeof value.native.available !== 'boolean') return null;
  result.native.available = value.native.available;
  if (value.native.available) {
    for (const key of ['installed', 'started', 'leaseActive', 'activationVerified', 'configurationPending', 'recoveryRequired', 'countControlAvailable']) {
      if (typeof value.native[key] !== 'boolean') return null;
      result.native[key] = value.native[key];
    }
    result.native.phase = phases.has(value.native.phase) ? value.native.phase : 'unknown';
    result.native.failureReason = nativeFailures.has(value.native.failureReason) ? value.native.failureReason : 'internal_failure';
    const activation = value.native.activation;
    if (object(activation) && activationReasons.has(activation.reason) && Number.isSafeInteger(activation.systemError) && activation.systemError >= 0 && activation.systemError <= 0xffffffff && typeof activation.attempted === 'boolean' && typeof activation.windowsVerified === 'boolean' && typeof value.native.activationCurrent === 'boolean' && (!activation.windowsVerified || (activation.reason === '' && activation.systemError === 0))) {
      result.native.activation = { reason: activation.reason, systemError: activation.systemError, attempted: activation.attempted, windowsVerified: activation.windowsVerified };
      result.native.activationCurrent = value.native.activationCurrent;
    }
    const recovery = safeRecoveryDiagnostic(value.native.recovery);
    if (recovery) result.native.recovery = recovery;
  }
  for (const row of value.events) {
    const event = safeHostEvent(row);
    if (event) result.events.push(event);
  }
  const encoder = safeHostEncoderDiagnostic(value.encoder);
  if (encoder && Buffer.byteLength(JSON.stringify({ ...result, encoder })) <= 6144) result.encoder = encoder;
  // The optional recovery explanation never costs the base report its slot.
  if (Buffer.byteLength(JSON.stringify(result)) > 6144 && result.native.recovery) delete result.native.recovery;
  // Neither does the device observation: it is kept only while everything fits.
  const devices = safeDeviceStatus(value.devices);
  if (devices && Buffer.byteLength(JSON.stringify({ ...result, devices })) <= 6144) result.devices = devices;
  return Buffer.byteLength(JSON.stringify(result)) <= 6144 ? result : null;
}
