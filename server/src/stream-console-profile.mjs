import { HttpError } from './errors.mjs';
import { relocatableErrors } from './vm-relocate.mjs';

export const streamConsoleProfileProtocol = 'console-profile-v1';
const uuid = /^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/i;

// Only the first supported console family. A name is not permission to alter
// Windows: the Host must additionally resolve one exact PCI/CCD device and
// preserve the entry topology in its protected restoration journal.
export function standardConsoleModel(config) {
  if (!config || typeof config !== 'object' || Array.isArray(config) ||
      (config.arch !== undefined && config.arch !== 'x86_64') ||
      (config.args !== undefined && config.args !== '') ||
      Object.entries(config).some(([key, value]) => /^hostpci\d+$/.test(key) &&
        (typeof value !== 'string' || /(?:^|,)x-vga=(?:1|on|yes|true)(?:,|$)/i.test(value)))) return null;
  let model;
  if (config.vga !== undefined) {
    if (typeof config.vga !== 'string' || !config.vga || config.vga.length > 120) return null;
    const seen = new Set();
    for (const part of config.vga.split(',')) {
      let [key, value, extra] = part.split('=');
      if (extra !== undefined) return null;
      if (value === undefined) { value = key; key = 'type'; }
      if (seen.has(key)) return null;
      seen.add(key);
      if (key === 'type') { if (value !== 'std') return null; model = value; }
      else if (key === 'memory') { if (!/^[1-9]\d{0,2}$/.test(value) || Number(value) < 4 || Number(value) > 512) return null; }
      else if (key === 'clipboard') { if (value !== 'vnc') return null; }
      else return null;
    }
  }
  // PVE get_vga_properties selects std for these x86 Windows versions,
  // including with older machine versions. Do not infer defaults for other OSes.
  if (!model && !['wvista', 'w2k8', 'win7', 'win8', 'win10', 'win11'].includes(config.ostype)) return null;
  return 'std';
}

// A VM whose Proxmox display is none or a serial terminal, or whose
// passed-through GPU is the primary VGA (x-vga), has no picture for the
// Console to show. The Console
// says so and how to change it instead of opening a blank screen.
export function consoleDisplayMissing(config) {
  if (!config || typeof config !== 'object' || Array.isArray(config)) return false;
  if (Object.entries(config).some(([key, value]) => /^hostpci\d+$/.test(key) && typeof value === 'string' &&
      /(?:^|,)x-vga=(?:1|on|yes|true)(?:,|$)/i.test(value))) return true;
  if (typeof config.vga !== 'string') return false;
  const type = config.vga.split(',').map(part => part.split('=')).find(([key, value]) => value === undefined || key === 'type');
  const name = type ? (type[1] ?? type[0]) : '';
  return name === 'none' || /^serial[0-3]$/.test(name);
}

// Why a VM display is not the standard VGA the console proof needs (the
// failed condition of standardConsoleModel, first match). A fixed code for the
// administrator's records only; the stream itself goes on without a proof.
export function consoleModelIssue(config) {
  if (standardConsoleModel(config)) return null;
  if (!config || typeof config !== 'object' || Array.isArray(config)) return 'console_config_other';
  if (Object.entries(config).some(([key, value]) => /^hostpci\d+$/.test(key) && typeof value === 'string' &&
      /(?:^|,)x-vga=(?:1|on|yes|true)(?:,|$)/i.test(value))) return 'console_gpu_primary';
  if (config.args !== undefined && config.args !== '') return 'console_qemu_args';
  if (typeof config.vga === 'string') {
    const type = config.vga.split(',').map(part => part.split('=')).find(([key, value]) => value === undefined || key === 'type');
    const name = type ? (type[1] ?? type[0]) : '';
    if (name && name !== 'std') return 'console_vga_other';
  }
  if (config.vga === undefined && (config.arch === undefined || config.arch === 'x86_64')) return 'console_vga_default';
  return 'console_config_other';
}

// A VM streams whatever its Proxmox display
// is, and while Proxmox cannot be read. The Host's display manager v2 turns
// every display it does not own off for the stream and puts the saved layout
// back at its end; the proof only lets it name one of them as the QEMU console.
// Without a proof the stream starts without `consoleProfile` (and, for all
// monitors, without the group marker), which every Host already takes. These
// are the fixed reasons recorded for the administrator when that happens.
export const consoleProofMissingCodes = Object.freeze([
  'console_vga_other', 'console_gpu_primary', 'console_qemu_args', 'console_vga_default', 'console_config_other',
  'proxmox_not_configured', 'proxmox_unreachable', 'proxmox_timeout', 'proxmox_permission_denied', 'proxmox_certificate_error',
  'proxmox_request_failed', 'proxmox_invalid_response', 'proxmox_vm_not_found', 'proxmox_vm_not_running', 'proxmox_target_invalid',
  'proxmox_settings_changed', 'proxmox_read_failed', 'machine_uuid_unknown',
]);
// Proxmox read failures (proxmox.mjs) that only mean "not known right now".
const proxmoxReadCodes = new Map([
  ['proxmox_not_configured', 'proxmox_not_configured'], ['proxmox_unreachable', 'proxmox_unreachable'],
  ['proxmox_permission_denied', 'proxmox_permission_denied'], ['proxmox_certificate_error', 'proxmox_certificate_error'],
  ['proxmox_request_failed', 'proxmox_request_failed'], ['proxmox_invalid_response', 'proxmox_invalid_response'],
  ['proxmox_vm_not_found', 'proxmox_vm_not_found'], ['stream_profile_vm_unavailable', 'proxmox_vm_not_running'],
  ['invalid_proxmox_target', 'proxmox_target_invalid'], ['invalid_proxmox_vm', 'proxmox_target_invalid'],
]);
// Proxmox says the registered VM is another machine: never streamed.
const identityRefusals = new Set(['proxmox_machine_uuid_mismatch', 'proxmox_machine_uuid_missing']);
const proofReadMs = 10000;
const timedOut = Symbol('timed_out');

// connect-timing-v1 (tuning backlog #2): the Proxmox reads of a console-profile
// stream start when its connection is granted, beside the client's relay dial,
// instead of after it. The proof below still waits for the attached session,
// uses them only for the same VM and Proxmox configuration, and re-checks
// everything after them.
export function prefetchStreamConsoleIdentity(relay, session) {
  try {
    if (session.localPc || session.mode !== 'stream' || (session.trackState && !session.profileTracks) ||
        session.host.capabilities?.streamProfileProtocol !== streamConsoleProfileProtocol) return;
    const desktop = relay.store.assignedDesktop(session.userId, session.desktopId);
    // Without a recorded VM UUID the proof reads with the Host's own UUID.
    if (desktop?.kind !== 'vm' || !desktop.pve_available || !Number.isSafeInteger(desktop.pve_vmid) || !uuid.test(desktop.pve_machine_uuid ?? '')) return;
    const revision = relay.store.setting('proxmox');
    const promise = Promise.resolve(relay.proxmox.consoleIdentity(relay.config(), desktop.pve_vmid, { node: desktop.pve_node, expectedUuid: desktop.pve_machine_uuid }));
    promise.catch(() => {}); // Read (and its failure reported) by the proof only.
    session.consoleIdentity = { promise, revision, node: desktop.pve_node, vmid: desktop.pve_vmid, expectedUuid: desktop.pve_machine_uuid };
  } catch { /* The proof reads it itself. */ }
}

// { proof } with the exact-session proof, or { proof: null, missing } when the
// stream goes on without one (`missing` is one of consoleProofMissingCodes).
// Throws HttpError for a stream that must not start:
//   stream_profile_not_authorized  the session, its authorization, the Host's
//                                  capability or an idle VM is gone (the relay
//                                  names the reason; see consoleProofRefusal);
//   desktop_unavailable            Proxmox no longer lists the desktop's VM;
//   desktop_changed                the desktop's VM or Host registration changed;
//   host_machine_mismatch          Proxmox says the registered VM is another machine.
export async function streamConsoleProof(relay, session) {
  const current = () => relay.current(session) && relay.authorized(session) &&
    session.mode === 'stream' && session.ws?.readyState === 1 &&
    session.host.capabilities?.streamProfileProtocol === streamConsoleProfileProtocol &&
    !relay.displayBusy.has(session.desktopId) && !relay.powerBusy(session.desktopId);
  if (!current()) throw new HttpError(409, 'stream_profile_not_authorized');
  const desktop = relay.store.assignedDesktop(session.userId, session.desktopId);
  if (!desktop) throw new HttpError(409, 'stream_profile_not_authorized');
  if (desktop.kind !== 'vm' || !Number.isSafeInteger(desktop.pve_vmid)) throw new HttpError(409, 'desktop_changed');
  if (!desktop.pve_available) throw new HttpError(409, 'desktop_unavailable');
  const host = relay.store.hostConsoleBinding(session.host.tokenHash);
  if (!host || host.id !== session.host.identity.id) throw new HttpError(409, 'desktop_changed');
  const prefetched = session.consoleIdentity; session.consoleIdentity = null;
  if (!uuid.test(host.machine_uuid ?? '')) return { proof: null, missing: 'machine_uuid_unknown' };
  const revision = relay.store.setting('proxmox');
  // Where the VM was read: the desktop's place, or the new one when it moved
  // (migration, HA, restore to a new VM ID) and was found again by its UUID
  // within this same wait (vm-relocate.mjs).
  const located = { node: desktop.pve_node, vmid: desktop.pve_vmid };
  let identity = null, failure = null, timer;
  try {
    const first = prefetched?.vmid === desktop.pve_vmid && prefetched.node === desktop.pve_node && prefetched.expectedUuid === desktop.pve_machine_uuid && prefetched.revision === revision
      ? prefetched.promise
      : Promise.resolve().then(() => relay.proxmox.consoleIdentity(relay.config(), desktop.pve_vmid, { node: desktop.pve_node, expectedUuid: host.machine_uuid }));
    const waitMs = relay.consoleProofReadMs ?? proofReadMs;
    const read = first.catch(async error => {
      if (!relay.vmRelocation || !relocatableErrors.has(error?.code) || relay.store.setting('proxmox') !== revision) throw error;
      const config = relay.config();
      const moved = await relay.vmRelocation.relocateDesktop(config, session.desktopId, located, error.code, { revision, signal: AbortSignal.timeout(waitMs) });
      if (!moved) throw error;
      located.node = moved.node; located.vmid = moved.vmid;
      return relay.proxmox.consoleIdentity(config, moved.vmid, { node: moved.node, expectedUuid: host.machine_uuid });
    });
    read.catch(() => {}); // A read that outlives the wait below is only dropped.
    identity = await Promise.race([read, new Promise(resolve => { timer = setTimeout(resolve, waitMs, timedOut); })]);
  } catch (error) { failure = error; }
  finally { clearTimeout(timer); }
  // Re-check every mutable authorization after network I/O. Proof is private to
  // this exact host/session and never returned from a client/admin response.
  const refreshed = relay.store.hostConsoleBinding(session.host.tokenHash);
  const currentDesktop = relay.store.assignedDesktop(session.userId, session.desktopId);
  if (!current() || !currentDesktop) throw new HttpError(409, 'stream_profile_not_authorized');
  if (!refreshed || refreshed.id !== host.id || refreshed.machine_uuid !== host.machine_uuid ||
      currentDesktop.pve_vmid !== located.vmid || currentDesktop.pve_node !== located.node || currentDesktop.pve_machine_uuid !== desktop.pve_machine_uuid) throw new HttpError(409, 'desktop_changed');
  if (relay.store.setting('proxmox') !== revision) return { proof: null, missing: 'proxmox_settings_changed' };
  if (failure) {
    if (failure instanceof HttpError && identityRefusals.has(failure.code)) throw new HttpError(409, 'host_machine_mismatch');
    return { proof: null, missing: (failure instanceof HttpError && proxmoxReadCodes.get(failure.code)) || 'proxmox_read_failed' };
  }
  if (identity === timedOut) return { proof: null, missing: 'proxmox_timeout' };
  if (!identity || !uuid.test(identity.machineUuid ?? '')) return { proof: null, missing: 'proxmox_invalid_response' };
  if (identity.machineUuid.toLowerCase() !== host.machine_uuid.toLowerCase()) throw new HttpError(409, 'host_machine_mismatch');
  if (identity.model !== 'std') return { proof: null, missing: consoleProofMissingCodes.includes(identity.issue) ? identity.issue : 'console_config_other' };
  return { proof: { protocol: streamConsoleProfileProtocol, sessionId: session.id, machineUuid: identity.machineUuid.toLowerCase(), model: 'std' } };
}
// The stream.unavailable reason of a refused start (streamConsoleProof throws).
export function consoleProofRefusal(relay, session, code) {
  if (['desktop_unavailable', 'desktop_changed', 'host_machine_mismatch'].includes(code)) return code;
  // A power action took the VM, or the Host withdrew the capability meanwhile.
  return relay.powerBusy(session.desktopId) ? 'power_in_progress' : 'host_maintenance';
}
