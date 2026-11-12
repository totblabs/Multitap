import { randomUUID } from 'node:crypto';
import { HttpError, fields } from './errors.mjs';
import { newToken, tokenHash, text } from './security.mjs';

// An old/manual pc row alone is never authority. Only the exact Host approved
// through its visible request code can authenticate, be assigned or connect.
export const approvedPcSql = `d.kind='pc' AND d.pve_vmid IS NULL AND d.enabled=1 AND EXISTS (
  SELECT 1 FROM local_pc_approvals lp JOIN hosts ph ON ph.id=lp.host_id
  WHERE lp.desktop_id=d.id AND ph.desktop_id=d.id AND ph.revoked=0
    AND lower(ph.machine_uuid)=lp.machine_uuid)`;
export const availableDesktopSql = `((d.kind='vm' AND d.pve_vmid IS NOT NULL AND d.pve_available=1) OR (${approvedPcSql}))`;
const pcId = /^pc-[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$/;
export function localPcCapabilities(capabilities) {
  const { streamProfileProtocol: _console, streamProfileTracksProtocol: _tracks, streamProfileViewportProtocol: _viewport, ...remaining } = capabilities;
  return remaining;
}

// lookup: where Proxmox places the machine (HostRequests.vmLookup). A machine
// that Proxmox itself lists as a VM is not offered as a local PC either.
export function pcRegistration(requests, row, candidates, lookup = null) {
  const previous = requests.store.db.prepare('SELECT d.id,d.name FROM local_pc_approvals p JOIN desktops d ON d.id=p.desktop_id WHERE p.machine_uuid=? AND d.kind=\'pc\'').get(row.machine_uuid);
  const knownVm = requests.store.db.prepare("SELECT 1 FROM desktops d WHERE d.kind='vm' AND d.pve_vmid IS NOT NULL AND (lower(d.pve_machine_uuid)=? OR EXISTS (SELECT 1 FROM hosts h WHERE h.desktop_id=d.id AND lower(h.machine_uuid)=?))").get(row.machine_uuid,row.machine_uuid);
  const liveVm = ['matched', 'ambiguous'].includes(lookup?.state);
  const allowed = candidates.length === 0 && !knownVm && !liveVm && requests.codeHosts.has(row.id);
  return { allowed, reason: candidates.length || knownVm || liveVm ? 'host_request_matches_vm' : allowed ? null : 'host_request_code_required',
    ...(previous ? { existingDesktopId: previous.id, existingName: previous.name } : {}) };
}

export async function approveLocalPc(requests, id, input, authorize) {
  requests.commissioned();
  fields(input, ['name', 'acknowledgeCode', 'acknowledgeUnlinked', 'replaceExisting', 'desktopId']);
  if (input.acknowledgeUnlinked !== true) throw new HttpError(400, 'local_pc_unlinked_confirmation_required');
  if (input.acknowledgeCode !== true || (input.replaceExisting !== undefined && typeof input.replaceExisting !== 'boolean') ||
      (input.desktopId !== undefined && (typeof input.desktopId !== 'string' || !pcId.test(input.desktopId)))) throw new HttpError(400, 'invalid_local_pc_approval');
  let name; try { name = text(input.name, 80); } catch { throw new HttpError(400, 'invalid_local_pc_name'); }
  authorize(); const first = requests.request(id);
  if (!requests.codeHosts.has(id)) throw new HttpError(409, 'host_request_code_required');
  requests.assertSingleClaim(first, true);
  const target = input.desktopId ?? `pc-${id}`, replacing = input.replaceExisting === true;
  const repeated = requests.approved(first, target, replacing); if (repeated) return repeated;
  if (first.status !== 'pending') throw new HttpError(409, 'host_request_' + first.status);
  const revision = requests.store.setting('proxmox');
  authorize(); requests.commissioned();
  if (requests.store.setting('proxmox') !== revision) throw new HttpError(409, 'proxmox_settings_changed');
  const knownVm = () => requests.store.db.prepare("SELECT 1 FROM desktops d WHERE d.kind='vm' AND d.pve_vmid IS NOT NULL AND (lower(d.pve_machine_uuid)=? OR EXISTS (SELECT 1 FROM hosts h WHERE h.desktop_id=d.id AND lower(h.machine_uuid)=?))").get(first.machine_uuid,first.machine_uuid) || requests.liveVm(first);
  if (knownVm()) throw new HttpError(409, 'host_request_matches_vm');
  const result = requests.store.transaction(() => {
    const actor = authorize(), now = requests.now(), row = requests.request(id);
    if (requests.store.setting('proxmox') !== revision) throw new HttpError(409, 'proxmox_settings_changed');
    const again = requests.approved(row, target, replacing); if (again) return again;
    if (row.status !== 'pending' || row.expires_at <= now) throw new HttpError(409, 'host_request_conflict');
    if (!requests.codeHosts.has(id)) throw new HttpError(409, 'host_request_code_required');
    requests.assertSingleClaim(row, true, now);
    if (['key_hash','machine_uuid','hostname','version','created_at','expires_at'].some(key => row[key] !== first[key])) throw new HttpError(409, 'host_request_conflict');
    if (knownVm()) throw new HttpError(409, 'host_request_matches_vm');
    const prior = requests.store.db.prepare('SELECT * FROM local_pc_approvals WHERE machine_uuid=?').get(row.machine_uuid);
    if (prior && (!replacing || prior.desktop_id !== target || input.desktopId !== target)) throw new HttpError(409, 'local_pc_already_registered');
    if (!prior && (replacing || input.desktopId !== undefined)) throw new HttpError(409, 'local_pc_registration_changed');
    if (prior && !requests.store.db.prepare("SELECT 1 FROM desktops WHERE id=? AND kind='pc' AND pve_vmid IS NULL").get(target)) throw new HttpError(409, 'local_pc_registration_changed');
    if (!prior && requests.store.db.prepare('SELECT 1 FROM desktops WHERE id=?').get(target)) throw new HttpError(409, 'local_pc_registration_changed');
    const hostId = randomUUID(), hostToken = newToken(), credentials = { hostId, hostToken, desktopId: target };
    const sealed = requests.vault.seal({ kind: 'host-request-v1', requestId: id, credentials });
    if (!prior) requests.store.db.prepare("INSERT INTO desktops(id,name,kind,description,os,enabled,created_at) VALUES(?,?,'pc','관리자가 승인한 로컬 PC','Windows',1,?)").run(target, name, now);
    else requests.store.db.prepare('UPDATE desktops SET name=?,enabled=1 WHERE id=?').run(name, target);
    requests.store.db.prepare("UPDATE host_requests SET status='rejected',credentials_sealed=NULL WHERE desktop_id=? AND status='approved'").run(target);
    // A PC row is found by its machine UUID, so a replacement is always the
    // same machine and keeps its assignments; a removed PC lost them at removal.
    requests.store.replacingHost(target, row.machine_uuid, actor, now);
    requests.store.db.prepare('DELETE FROM hosts WHERE desktop_id=?').run(target);
    requests.store.db.prepare('INSERT INTO hosts(id,desktop_id,token_hash,hostname,machine_uuid,version,created_at) VALUES(?,?,?,?,?,?,?)').run(hostId, target, tokenHash(hostToken), row.hostname, row.machine_uuid, row.version, now);
    requests.store.db.prepare('INSERT INTO local_pc_approvals(desktop_id,host_id,machine_uuid,approved_at,actor) VALUES(?,?,?,?,?) ON CONFLICT(desktop_id) DO UPDATE SET host_id=excluded.host_id,machine_uuid=excluded.machine_uuid,approved_at=excluded.approved_at,actor=excluded.actor').run(target, hostId, row.machine_uuid, now, actor);
    requests.store.db.prepare("UPDATE host_requests SET status='approved',desktop_id=?,host_id=?,credentials_sealed=?,decision_actor=?,decided_at=?,replace_existing=? WHERE id=? AND status='pending'").run(target, hostId, sealed, actor, now, replacing ? 1 : 0, id);
    requests.store.audit('local_pc_approved', actor, target, now); requests.store.audit('host_enrolled', hostId, target, now);
    return { ok: true, requestId: id, status: 'approved', desktopId: target };
  });
  requests.changed(); return result;
}
