import { WebSocket } from 'ws';
import { safeDiagnosticReport } from './host-diagnostics.mjs';
import { safeClientDiagnosticReport, clientDiagnosticRetentionMs, streamStatsProtocol } from './client-diagnostics.mjs';
import { availableDesktopSql } from './local-pc.mjs';

// Read-only administrator views for the console's monitoring pages. They
// project relay/store state into fixed fields: no tokens, token hashes,
// machine identities, credentials or free text.

const managedIds = store => new Set(store.db.prepare(`SELECT d.id FROM desktops d WHERE ${availableDesktopSql}`).all().map(row => row.id));

// Current relay sessions (pending and attached). The relay does not record
// a start time, so none is reported; the console correlates client records.
export function activeSessions(store, relay) {
  const names = new Map(store.db.prepare('SELECT id,username FROM users').all().map(row => [row.id, row.username]));
  const managed = managedIds(store);
  const sessions = [...(relay?.sessions?.values() ?? [])].filter(session => managed.has(session.desktopId)).map(session => {
    return { sessionId: session.id, desktopId: session.desktopId, account: names.get(session.userId) ?? null, mode: 'stream', attached: !!session.ws, ready: session.streamReady === true };
  }).sort((a, b) => a.desktopId.localeCompare(b.desktopId, 'en', { numeric: true }) || String(a.account).localeCompare(String(b.account), 'ko'));
  // The UDP media relay's counters and its socket buffers (fixed numeric fields), only where one is configured.
  return { sessions, ...(relay?.udp ? { udpRelay: relay.udp.stats() } : {}), checkedAt: new Date().toISOString() };
}

// Enrolled (not revoked) Hosts of managed VMs with connection state and a
// digest of the latest automatic diagnostics report.
export function enrolledHosts(store, relay, now = Date.now()) {
  const managed = store.registeredHostIds();
  const rows = store.db.prepare('SELECT h.id,h.desktop_id,h.hostname,h.version,h.created_at,h.last_seen_at,d.received_at,d.report FROM hosts h LEFT JOIN host_diagnostics d ON d.host_id=h.id WHERE h.revoked=0 ORDER BY h.desktop_id').all();
  const hosts = [];
  for (const row of rows) {
    if (!managed.has(row.desktop_id)) continue;
    const live = relay?.hosts?.get(row.desktop_id), connected = live?.identity?.id === row.id && live?.ws?.readyState === WebSocket.OPEN && relay?.hostAuthenticated?.(live) === true;
    let report = null;
    try { report = row.report ? safeDiagnosticReport(JSON.parse(row.report)) : null; } catch { report = null; }
    const version = connected && typeof live.identity?.version === 'string' ? live.identity.version : row.version;
    hosts.push({ hostId: row.id, desktopId: row.desktop_id, hostname: row.hostname, version: /^\d{1,6}\.\d{1,6}\.\d{1,6}$/.test(version ?? '') ? version : null, enrolledAt: new Date(row.created_at).toISOString(),
      lastSeenAt: connected && live.lastSeen ? new Date(live.lastSeen).toISOString() : row.last_seen_at ? new Date(row.last_seen_at).toISOString() : null, connected,
      diagnostics: report ? { receivedAt: new Date(row.received_at).toISOString(), stale: !connected || now - row.received_at > 90000, version: report.version, collectedAt: report.collectedAt,
        interactiveSession: report.interactiveSession, consoleSession: report.consoleSession,
        display: { observed: report.display.observed, monitorCount: report.display.monitorCount, installed: report.display.installed, deviceReady: report.display.deviceReady, owned: report.display.owned, activeDisplay: report.display.activeDisplay, rebootRequired: report.display.rebootRequired, recoveryRequired: report.display.recoveryRequired, reason: report.display.reason },
        native: report.native.available ? { available: true, phase: report.native.phase, failureReason: report.native.failureReason, leaseActive: report.native.leaseActive, activationVerified: report.native.activationVerified } : { available: false },
        events: report.events } : null });
  }
  return { hosts, checkedAt: new Date(now).toISOString() };
}

// Streaming statistics windows grouped by session, newest sessions first and
// windows in time order. Bounded to the newest `limit` windows overall.
export function streamStatsSessions(store, now = Date.now(), limit = 480) {
  const batches = store.db.prepare('SELECT d.received_at,d.report,u.username FROM client_diagnostics d JOIN users u ON u.id=d.user_id WHERE d.received_at>=? ORDER BY d.id DESC LIMIT 512').all(now - clientDiagnosticRetentionMs);
  const rows = [];
  for (const batch of batches) {
    let report; try { report = safeClientDiagnosticReport(JSON.parse(batch.report)); } catch { report = null; }
    if (!report) continue;
    for (const event of report.events) {
      if (event.event !== 'streamStats') continue;
      rows.push({ key: event.sessionId ?? `run:${report.runId}:${event.desktopId ?? ''}`, sessionId: event.sessionId ?? null, desktopId: event.desktopId ?? null, account: batch.username, version: report.version, time: event.time, receivedAt: new Date(batch.received_at).toISOString(), values: event.values });
    }
  }
  rows.sort((a, b) => Date.parse(b.time) - Date.parse(a.time));
  const groups = new Map();
  for (const row of rows.slice(0, limit)) {
    if (!groups.has(row.key)) groups.set(row.key, { sessionId: row.sessionId, desktopId: row.desktopId, account: row.account, version: row.version, windows: [] });
    groups.get(row.key).windows.push({ time: row.time, receivedAt: row.receivedAt, values: row.values });
  }
  const sessions = [...groups.values()].map(group => ({ ...group, windows: group.windows.reverse() }));
  return { protocol: streamStatsProtocol, retentionDays: 7, limit, truncated: rows.length > limit, sessions };
}
