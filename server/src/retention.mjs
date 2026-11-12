import { clientDiagnosticRetentionMs, clientDiagnosticBatchesPerUser, clientDiagnosticBatchesTotal, boundClientDiagnostics } from './client-diagnostics.mjs';
import { hostEventHistoryMs } from './host-diagnostics.mjs';
import { usageRetentionMs, usageRowsKept } from './usage.mjs';
import { noticeRetentionRules } from './notices.mjs';

// Retention of the broker's diagnostic and audit tables. Every limit keeps at
// least what the administrator console says it shows.
// - client_diagnostics: 7 days; 1024 batches per account and 4096 in all,
//   enforced on each upload (client-diagnostics.mjs) and finished here.
// - host_diagnostics: the latest report of each enrolled Host, whatever its age
//   (the console shows it as stale); a revoked Host's report is never shown.
// - host_diagnostic_events: 7 days and 500 per Host (host-diagnostics.mjs); a
//   revoked Host's history is never shown.
// - audit_events: 180 days and at most 500,000 rows; the newest 100 (관리 기록
//   in the console) always stay.
// - usage_logins, usage_connections (모니터링, usage.mjs): 2 years
//   and at most 2,000,000 rows each, the oldest first.
//   A sign-in or connection that is still running is never deleted.
// - notices (공지, notices.mjs): 180 days and at most 2,000 rows; one that still
//   shows is never deleted. Who received a notice goes 30 days after its end;
//   the counts stay on the notice.
export const auditRetentionMs = 180 * 86400000, auditRowsKept = 500000, auditNewestKept = 100;
// One step deletes at most this many rows from one table. Client batches are up
// to 16 KB each, so they go 32 at a time (p95 ~2 ms per step at 100k rows).
export const retentionBatch = 500, clientDiagnosticBatch = 32;

// Ids below this one may go: everything but the newest auditNewestKept rows.
const auditCutoff = store => store.db.prepare(`SELECT id FROM audit_events ORDER BY id DESC LIMIT 1 OFFSET ${auditNewestKept - 1}`).get()?.id ?? 0;
const rules = [
  (store, now) => store.pruneReplacedSessions(now, retentionBatch),
  (store, now) => store.db.prepare('DELETE FROM client_diagnostics WHERE id IN (SELECT id FROM client_diagnostics WHERE received_at<? LIMIT ?)').run(now - clientDiagnosticRetentionMs, clientDiagnosticBatch).changes,
  store => boundClientDiagnostics(store, null, clientDiagnosticBatchesTotal, clientDiagnosticBatch),
  store => {
    const over = store.db.prepare('SELECT user_id FROM client_diagnostics GROUP BY user_id HAVING count(*)>? LIMIT 1').get(clientDiagnosticBatchesPerUser);
    return over ? boundClientDiagnostics(store, over.user_id, clientDiagnosticBatchesPerUser, clientDiagnosticBatch) : 0;
  },
  store => store.db.prepare('DELETE FROM host_diagnostics WHERE host_id IN (SELECT d.host_id FROM host_diagnostics d JOIN hosts h ON h.id=d.host_id WHERE h.revoked=1 LIMIT ?)').run(retentionBatch).changes,
  (store, now) => store.db.prepare('DELETE FROM host_diagnostic_events WHERE id IN (SELECT id FROM host_diagnostic_events WHERE received_at<? LIMIT ?)').run(now - hostEventHistoryMs, retentionBatch).changes,
  store => store.db.prepare('DELETE FROM host_diagnostic_events WHERE id IN (SELECT e.id FROM host_diagnostic_events e JOIN hosts h ON h.id=e.host_id WHERE h.revoked=1 LIMIT ?)').run(retentionBatch).changes,
  (store, now) => store.db.prepare('DELETE FROM audit_events WHERE id IN (SELECT id FROM audit_events WHERE created_at<? AND id<? LIMIT ?)').run(now - auditRetentionMs, auditCutoff(store), retentionBatch).changes,
  (store, now) => store.db.prepare('DELETE FROM usage_logins WHERE id IN (SELECT id FROM usage_logins WHERE started_at<? AND token_hash IS NULL LIMIT ?)').run(now - usageRetentionMs, retentionBatch).changes,
  (store, now) => store.db.prepare('DELETE FROM usage_connections WHERE id IN (SELECT id FROM usage_connections WHERE started_at<? AND ended_at IS NOT NULL LIMIT ?)').run(now - usageRetentionMs, retentionBatch).changes,
  // Ids only grow (AUTOINCREMENT), so the newest usageRowsKept ids are at most that many rows.
  ...['usage_logins WHERE token_hash IS NULL', 'usage_connections WHERE ended_at IS NOT NULL'].map(scope => store => {
    const table = scope.split(' ')[0], { last } = store.db.prepare(`SELECT max(id) last FROM ${table}`).get();
    return last > usageRowsKept ? store.db.prepare(`DELETE FROM ${table} WHERE id IN (SELECT id FROM ${scope} AND id<=? ORDER BY id LIMIT ?)`).run(last - usageRowsKept, retentionBatch).changes : 0;
  }),
  ...noticeRetentionRules(retentionBatch),
  // Audit ids only grow and only the oldest are deleted, so the newest
  // auditRowsKept ids are at most that many rows (no count(*) scan).
  store => {
    const { last } = store.db.prepare('SELECT max(id) last FROM audit_events').get();
    return last > auditRowsKept ? store.db.prepare('DELETE FROM audit_events WHERE id IN (SELECT id FROM audit_events WHERE id<=? AND id<? ORDER BY id LIMIT ?)').run(last - auditRowsKept, auditCutoff(store), retentionBatch).changes : 0;
  },
];

// Deletes one small batch from the first table that has expired rows; false
// once nothing is left to delete.
export function pruneStep(store, now = Date.now()) {
  for (const rule of rules) if (Number(rule(store, now)) > 0) return true;
  return false;
}

// Hourly, while no remote session is running; after a day without such a
// moment it runs anyway. Steps yield to the event loop between batches.
export class RetentionSweeper {
  constructor({ store, idle = () => true, intervalMs = 3600000, forceAfterMs = 86400000, now = Date.now }) {
    Object.assign(this, { store, idle, intervalMs, forceAfterMs, now, timer: null, closed: false });
    this.lastSweep = now();
  }
  start() { this.schedule(); return this; }
  schedule() {
    clearTimeout(this.timer);
    if (this.closed) return;
    this.timer = setTimeout(() => void this.tick(), this.intervalMs); this.timer.unref?.();
  }
  async tick() {
    if (!this.closed && (this.idle() || this.now() - this.lastSweep >= this.forceAfterMs)) await this.sweep();
    this.schedule();
  }
  async sweep() {
    let steps = 0;
    try { while (!this.closed && pruneStep(this.store, this.now())) { steps++; await new Promise(resolve => setImmediate(resolve)); } }
    catch { /* The rows stay for the next sweep. */ }
    this.lastSweep = this.now();
    return steps;
  }
  close() { this.closed = true; clearTimeout(this.timer); }
}
