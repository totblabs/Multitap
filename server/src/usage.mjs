import { safeClientDiagnosticReport } from './client-diagnostics.mjs';

// Usage history for the console's 모니터링 page: who signed in, and who was
// connected to which desktop from when until when. The broker knows connected
// time, never work: nothing here measures what a person did.
//
// usage_logins       one row per client sign-in (purpose 'client': the Windows
//                    and Android apps and the browser console; never the
//                    administrator console). Written by triggers on `sessions`,
//                    so every place that creates or removes a sign-in is
//                    covered and none of them changed.
// usage_connections  one row per relayed connection, from the moment the
//                    client's socket attaches until the relay lets go of it
//                    (UsageRecorder, called from relay.mjs).
//
// Both are additive (CREATE ... IF NOT EXISTS, no schema version change): an
// older broker ignores the tables, and its sign-ins still pass the triggers.
// Kept for 2 years and at most usageRowsKept rows each (retention.mjs); an
// account's rows go with the account (ON DELETE CASCADE).
//
// Address: what request-origin's requestAddress yields, an exact IPv4 address
// or the /64 prefix of an IPv6 one. Nothing more is stored than is shown.
// token_hash: only while the sign-in is live (it joins `sessions`, which holds
// the same hash); cleared when the sign-in ends.
export const usageRetentionMs = 730 * 86400000, usageRetentionDays = 730, usageRowsKept = 2000000;
export const usageProtocol = 'usage-v1';
// round, not CAST alone: unixepoch('subsec')*1000 can come out as …999.9999 and a cast then
// records a sign-out one millisecond early (before the moment it happened).
const nowSql = "CAST(round(unixepoch('subsec')*1000) AS INTEGER)";
const platformSql = issuer => `CASE ${issuer} WHEN 'windows-pc-v1' THEN 'windows' WHEN 'browser-console' THEN 'web' WHEN 'legacy' THEN 'android' END`;
export const usageSchema = `
CREATE TABLE IF NOT EXISTS usage_logins (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  token_hash TEXT,
  started_at INTEGER NOT NULL, ended_at INTEGER, end_reason TEXT,
  estimated INTEGER NOT NULL DEFAULT 0 CHECK(estimated IN (0,1)),
  issuer TEXT, platform TEXT, client_version TEXT, address TEXT,
  source TEXT NOT NULL DEFAULT 'server' CHECK(source IN ('server','audit'))
);
CREATE UNIQUE INDEX IF NOT EXISTS usage_logins_token ON usage_logins(token_hash) WHERE token_hash IS NOT NULL;
CREATE INDEX IF NOT EXISTS usage_logins_started ON usage_logins(started_at);
CREATE INDEX IF NOT EXISTS usage_logins_user ON usage_logins(user_id,started_at);
CREATE TABLE IF NOT EXISTS usage_connections (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  session_id TEXT NOT NULL UNIQUE,
  user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  desktop_id TEXT NOT NULL, desktop_name TEXT NOT NULL, mode TEXT NOT NULL,
  started_at INTEGER NOT NULL, last_seen_at INTEGER NOT NULL, ended_at INTEGER, end_reason TEXT,
  estimated INTEGER NOT NULL DEFAULT 0 CHECK(estimated IN (0,1)),
  platform TEXT, client_version TEXT, address TEXT,
  source TEXT NOT NULL DEFAULT 'server' CHECK(source IN ('server','client'))
);
CREATE INDEX IF NOT EXISTS usage_connections_started ON usage_connections(started_at);
CREATE INDEX IF NOT EXISTS usage_connections_user ON usage_connections(user_id,started_at);
CREATE INDEX IF NOT EXISTS usage_connections_desktop ON usage_connections(desktop_id,started_at);
CREATE INDEX IF NOT EXISTS usage_connections_open ON usage_connections(started_at) WHERE ended_at IS NULL;
CREATE TRIGGER IF NOT EXISTS usage_login_started AFTER INSERT ON sessions WHEN NEW.purpose='client' BEGIN
  INSERT OR IGNORE INTO usage_logins(user_id,token_hash,started_at,issuer,platform)
    VALUES(NEW.user_id,NEW.token_hash,NEW.created_at,NEW.issuer,${platformSql('NEW.issuer')});
END;
CREATE TRIGGER IF NOT EXISTS usage_login_ended AFTER DELETE ON sessions WHEN OLD.purpose='client' BEGIN
  UPDATE usage_logins SET ended_at=max(started_at,min(${nowSql},OLD.expires_at)),
    end_reason=CASE WHEN OLD.expires_at<=${nowSql} THEN 'expired' ELSE 'ended' END
    WHERE token_hash=OLD.token_hash AND ended_at IS NULL;
END;
CREATE TRIGGER IF NOT EXISTS usage_login_reason AFTER INSERT ON session_end_reasons BEGIN
  UPDATE usage_logins SET end_reason=NEW.reason WHERE token_hash=NEW.token_hash;
END;
`;

const reasonPattern = /^[a-z][a-z0-9_]{0,47}$/, versionPattern = /^\d{1,6}\.\d{1,6}\.\d{1,6}(?:\+\d{1,10})?$/;
const hashPattern = /^[a-f0-9]{64}$/, sessionIdPattern = /^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/i;
const safeAddress = value => typeof value === 'string' && value !== 'unknown' && /^[0-9a-f.:/]{2,64}$/i.test(value) ? value : null;
const safeVersion = value => typeof value === 'string' && versionPattern.test(value) ? value : null;
const forgetEndedTokens = store => store.db.prepare('UPDATE usage_logins SET token_hash=NULL WHERE token_hash IS NOT NULL AND ended_at IS NOT NULL').run();

// Tables, triggers and the one-time backfill; called by the Store on every start.
export function installUsage(store, now = Date.now()) {
  // A trigger made by an earlier broker with the truncating clock is replaced (the schema keeps IF NOT EXISTS).
  const ended = store.db.prepare("SELECT sql FROM sqlite_master WHERE type='trigger' AND name='usage_login_ended'").get();
  if (ended && !ended.sql.includes(nowSql)) store.db.exec('DROP TRIGGER usage_login_ended');
  store.db.exec(usageSchema);
  store.transaction(() => {
    if (store.setting('usage_backfill') !== undefined) return;
    const counts = backfillUsage(store, now);
    store.db.prepare("INSERT INTO settings(key,value) VALUES('usage_backfill',?)").run(JSON.stringify({ at: now, ...counts }));
  });
}

// What the broker already held when recording started, once:
// - the sign-ins that are live now (exact start, from `sessions`);
// - earlier sign-ins of user accounts from audit_events (login_succeeded, at
//   most 180 days back). An administrator account's audit sign-ins cannot be
//   told from its console sign-ins, so they are left out rather than counted
//   wrong. A sign-in's end is paired from the same account's next logout,
//   idle logout or replacement and marked estimated; unpaired ones stay open-ended.
// - connections of the last 7 days from client reports (client_diagnostics):
//   first and last report of each session id, client clock, marked estimated.
export function backfillUsage(store, now = Date.now()) {
  const db = store.db;
  const live = db.prepare(`INSERT OR IGNORE INTO usage_logins(user_id,token_hash,started_at,issuer,platform)
    SELECT user_id,token_hash,created_at,issuer,${platformSql('issuer')} FROM sessions WHERE purpose='client' AND replaced_at IS NULL AND expires_at>?`).run(now).changes;
  const known = new Set(db.prepare('SELECT user_id,started_at FROM usage_logins').all().map(row => row.user_id + ':' + row.started_at));
  const insertLogin = db.prepare("INSERT INTO usage_logins(user_id,started_at,ended_at,end_reason,estimated,source) VALUES(?,?,?,?,?,'audit')");
  const open = new Map(), finished = [];
  const close = (entry, at, reason) => { entry.endedAt = Math.max(entry.startedAt, at); entry.reason = reason; };
  for (const row of db.prepare(`SELECT a.created_at,a.event,a.actor FROM audit_events a JOIN users u ON u.id=a.actor
      WHERE a.event IN ('login_succeeded','logout','client_idle_logout','client_login_replaced') AND u.role='user' ORDER BY a.id`).iterate()) {
    const pending = open.get(row.actor) ?? [];
    if (row.event === 'login_succeeded') {
      if (known.has(row.actor + ':' + row.created_at)) continue;
      const entry = { userId: row.actor, startedAt: row.created_at, endedAt: null, reason: null };
      pending.push(entry); finished.push(entry); open.set(row.actor, pending);
    } else if (row.event === 'client_login_replaced') { for (const entry of pending) close(entry, row.created_at, 'pc_login_replaced'); open.delete(row.actor); }
    else if (pending.length) close(pending.pop(), row.created_at, row.event === 'logout' ? 'logout' : 'session_idle_timeout');
  }
  for (const entry of finished) insertLogin.run(entry.userId, entry.startedAt, entry.endedAt, entry.reason, entry.endedAt === null ? 0 : 1);

  const sessions = new Map();
  for (const batch of db.prepare('SELECT user_id,received_at,report FROM client_diagnostics WHERE received_at>=? ORDER BY id DESC LIMIT 4096').iterate(now - 7 * 86400000)) {
    let report; try { report = safeClientDiagnosticReport(JSON.parse(batch.report)); } catch { report = null; }
    if (!report) continue;
    for (const event of report.events) {
      if (!event.sessionId || !event.desktopId || !['streamReady', 'streamStats', 'streamEnd'].includes(event.event) || event.code) continue;
      const at = Date.parse(event.time);
      // A client clock far from the server's receipt time says nothing usable.
      if (!(at <= batch.received_at + 300000 && at >= batch.received_at - 86400000)) continue;
      const key = event.sessionId.toLowerCase(), entry = sessions.get(key) ?? { userId: batch.user_id, desktopId: event.desktopId, mode: 'stream', first: at, last: at, version: safeVersion(report.version) };
      if (entry.userId !== batch.user_id) continue;
      entry.first = Math.min(entry.first, at); entry.last = Math.max(entry.last, at); sessions.set(key, entry);
    }
  }
  const names = new Map(db.prepare('SELECT id,name FROM desktops').all().map(row => [row.id, row.name]));
  const insertConnection = db.prepare(`INSERT OR IGNORE INTO usage_connections(session_id,user_id,desktop_id,desktop_name,mode,started_at,last_seen_at,ended_at,end_reason,estimated,client_version,source)
    VALUES(?,?,?,?,?,?,?,?,'unknown',1,?,'client')`);
  let connections = 0;
  for (const [id, entry] of [...sessions].sort((a, b) => a[1].first - b[1].first)) {
    connections += Number(insertConnection.run(id, entry.userId, entry.desktopId, names.get(entry.desktopId) ?? entry.desktopId, entry.mode, entry.first, entry.last, entry.last, entry.version).changes);
  }
  return { liveLogins: Number(live), auditLogins: finished.length, clientConnections: connections };
}

// A sign-in's annotations, where the broker learns them: the address of the
// request that signed in, and the version a client names in its own reports.
export function noteLogin(store, hash, { address } = {}) {
  try { if (hashPattern.test(hash ?? '') && safeAddress(address)) store.db.prepare('UPDATE usage_logins SET address=? WHERE token_hash=?').run(safeAddress(address), hash); } catch { /* History never blocks a sign-in. */ }
}
export function noteLoginEnd(store, hash, reason) {
  try {
    if (hashPattern.test(hash ?? '') && reasonPattern.test(reason ?? '')) store.db.prepare("UPDATE usage_logins SET end_reason=? WHERE token_hash=? AND ended_at IS NOT NULL AND end_reason='ended'").run(reason, hash);
    forgetEndedTokens(store);
  } catch { /* History never blocks a sign-out. */ }
}
export function noteClientReport(store, hash, userId, report) {
  try {
    const version = safeVersion(report?.version);
    if (!version || !hashPattern.test(hash ?? '')) return;
    store.db.prepare('UPDATE usage_logins SET client_version=? WHERE token_hash=? AND user_id=? AND client_version IS NOT ?').run(version, hash, userId, version);
    const ids = new Set((Array.isArray(report.events) ? report.events : []).map(event => event?.sessionId).filter(id => typeof id === 'string' && sessionIdPattern.test(id)));
    for (const id of ids) store.db.prepare('UPDATE usage_connections SET client_version=? WHERE session_id=? AND user_id=? AND client_version IS NOT ?').run(version, id.toLowerCase(), userId, version);
  } catch { /* A report is stored whether or not history could use it. */ }
}
// Sign-ins whose `sessions` row says they are over although it still exists
// (expired, replaced), and any left without a row at all.
export function settleLogins(store, now = Date.now()) {
  const db = store.db;
  db.prepare(`UPDATE usage_logins SET end_reason='expired',ended_at=max(started_at,(SELECT s.expires_at FROM sessions s WHERE s.token_hash=usage_logins.token_hash))
    WHERE ended_at IS NULL AND token_hash IN (SELECT token_hash FROM sessions WHERE purpose='client' AND expires_at<=?)`).run(now);
  db.prepare(`UPDATE usage_logins SET end_reason='pc_login_replaced',ended_at=max(started_at,(SELECT s.replaced_at FROM sessions s WHERE s.token_hash=usage_logins.token_hash))
    WHERE ended_at IS NULL AND token_hash IN (SELECT token_hash FROM sessions WHERE replaced_at IS NOT NULL)`).run();
  db.prepare(`UPDATE usage_logins SET end_reason='unknown',estimated=1,ended_at=max(started_at,?)
    WHERE ended_at IS NULL AND token_hash IS NOT NULL AND NOT EXISTS (SELECT 1 FROM sessions s WHERE s.token_hash=usage_logins.token_hash)`).run(now);
  forgetEndedTokens(store);
}

// Endings the relay only notices some time after they happened.
const lateReasons = new Set(['client_timeout', 'client_backpressure', 'client_error', 'tracks_host_disconnected', 'tracks_source_timeout', 'tracks_source_stalled']);
// Connection start and end, exactly as the relay sees them.
// - start: the client's socket attached to its session;
// - end: the relay released the session, with its reason. A side that went
//   away without a word (the relay held the session for it and it never came
//   back) ends at the moment the relay lost it, marked estimated;
// - a broker that stopped without closing its sessions (crash, power loss)
//   leaves rows open: the next start ends them at their last heartbeat, marked
//   estimated. A connection's time therefore never grows after it died.
export class UsageRecorder {
  constructor({ store, relay = null, now = Date.now, heartbeatMs = 20000 }) {
    Object.assign(this, { store, relay, now, heartbeatMs, open: new Map(), addresses: new Map(), timer: null });
  }
  start() {
    this.recover();
    this.timer = setInterval(() => this.tick(), this.heartbeatMs); this.timer.unref?.();
    return this;
  }
  recover() {
    try {
      this.store.db.prepare("UPDATE usage_connections SET ended_at=max(started_at,last_seen_at),end_reason='server_restart',estimated=1 WHERE ended_at IS NULL").run();
      settleLogins(this.store, this.now());
    } catch { /* The next tick tries again. */ }
  }
  // The address of the request that asked for the connection (http.mjs).
  requested(sessionId, address) {
    if (!safeAddress(address) || typeof sessionId !== 'string') return;
    this.addresses.set(sessionId, safeAddress(address));
    if (this.addresses.size > 600) this.addresses.delete(this.addresses.keys().next().value);
  }
  attached(session) {
    try {
      // A returning socket of the same connection (session-resume-v1) is not a new one.
      if (!session || this.open.has(session.id)) return;
      const db = this.store.db, at = this.now();
      const login = db.prepare('SELECT platform,client_version,address FROM usage_logins WHERE token_hash=?').get(session.tokenHash);
      const name = db.prepare('SELECT name FROM desktops WHERE id=?').get(session.desktopId)?.name ?? session.desktopId;
      const address = this.addresses.get(session.id) ?? login?.address ?? null; this.addresses.delete(session.id);
      db.prepare(`INSERT OR IGNORE INTO usage_connections(session_id,user_id,desktop_id,desktop_name,mode,started_at,last_seen_at,platform,client_version,address)
        VALUES(?,?,?,?,?,?,?,?,?,?)`).run(session.id, session.userId, session.desktopId, String(name).slice(0, 200), 'stream', at, at,
        session.touchInput ? 'android' : login?.platform ?? null, login?.client_version ?? null, address);
      this.open.set(session.id, session);
    } catch { /* History never blocks a connection. */ }
  }
  ended(session, reason) {
    try {
      this.addresses.delete(session?.id);
      if (!session || !this.open.delete(session.id)) return;
      const at = this.now();
      const lost = [session.detached?.at, session.hostHeld?.at, session.trackHeld?.at].filter(Number.isFinite);
      const endedAt = lost.length ? Math.min(at, ...lost) : at;
      this.store.db.prepare('UPDATE usage_connections SET ended_at=max(started_at,?),last_seen_at=max(started_at,?),end_reason=?,estimated=? WHERE session_id=? AND ended_at IS NULL')
        .run(endedAt, endedAt, reasonPattern.test(reason ?? '') ? reason : 'unknown', lost.length || lateReasons.has(reason) ? 1 : 0, session.id);
    } catch { /* The next start closes the row at its last heartbeat. */ }
  }
  tick() {
    try {
      const at = this.now(), beat = this.store.db.prepare('UPDATE usage_connections SET last_seen_at=? WHERE session_id=? AND ended_at IS NULL');
      for (const [id, session] of this.open) {
        if (!this.relay || this.relay.sessions.get(id) === session) beat.run(at, id);
        // Released without passing ended(): the last heartbeat is the last proof of life.
        else { this.open.delete(id); this.store.db.prepare("UPDATE usage_connections SET ended_at=max(started_at,last_seen_at),end_reason='unknown',estimated=1 WHERE session_id=? AND ended_at IS NULL").run(id); }
      }
      settleLogins(this.store, at);
    } catch { /* The next tick tries again. */ }
  }
  close() { clearInterval(this.timer); this.timer = null; }
}

// ---- Read side (administrators only; admin-http.mjs). Days are the broker's
// local calendar days; every answer names the time zone.
const pad = value => String(value).padStart(2, '0');
export const dayKey = ms => { const d = new Date(ms); return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`; };
// Local midnight that starts the day `key` (YYYY-MM-DD) plus `offset` days; null for a date that does not exist.
export function dayStart(key, offset = 0) {
  const match = /^(\d{4})-(\d\d)-(\d\d)$/.exec(key ?? '');
  if (!match) return null;
  const [year, month, day] = match.slice(1).map(Number), date = new Date(year, month - 1, day);
  if (year < 2000 || year > 2200 || date.getFullYear() !== year || date.getMonth() !== month - 1 || date.getDate() !== day) return null;
  return offset ? new Date(year, month - 1, day + offset).getTime() : date.getTime();
}
export const usageMaxRangeDays = 366, usagePageSize = 200, usageMaxPage = 500, usageSummaryRows = 200000;
// A list of everyone's events is one day in the console; more than a month at
// once would sort tens of thousands of rows per page.
export const usageAllEventsDays = 31;
// The summary reads this many rows, then lets the broker's other work run.
export const usageSummaryChunk = 4000;
function range(from, to) {
  const start = dayStart(from), last = dayStart(to);
  if (start === null || last === null || last < start) return null;
  const days = [];
  for (let offset = 0; ; offset++) {
    const at = dayStart(from, offset);
    if (at > last) break;
    if (days.length >= usageMaxRangeDays) return null;
    days.push({ key: dayKey(at), start: at, end: dayStart(from, offset + 1) });
  }
  return { days, start, end: days.at(-1).end };
}
const zone = (now = Date.now()) => ({ timeZone: Intl.DateTimeFormat().resolvedOptions().timeZone ?? 'UTC', utcOffsetMinutes: -new Date(now).getTimezoneOffset() });
const iso = value => Number.isFinite(value) ? new Date(value).toISOString() : null;
const liveIds = relay => new Set([...(relay?.sessions?.values() ?? [])].filter(session => session.ws || session.detached).map(session => session.id));
const recordingSince = store => { try { return iso(JSON.parse(store.setting('usage_backfill') ?? 'null')?.at); } catch { return null; } };
const sequence = (store, table) => Number(store.db.prepare('SELECT seq FROM sqlite_sequence WHERE name=?').get(table)?.seq ?? 0);

// 지금: live sign-ins and live connections, today's counts and the running totals.
export function usageLive(store, relay, now = Date.now()) {
  const db = store.db, today = dayKey(now), start = dayStart(today), end = dayStart(today, 1);
  const sessions = [...(relay?.sessions?.values() ?? [])].filter(session => session.ws || session.detached);
  const names = new Map(db.prepare('SELECT id,username FROM users').all().map(row => [row.id, row.username]));
  const recorded = new Map(db.prepare('SELECT session_id,started_at,desktop_name,platform,client_version,address FROM usage_connections WHERE ended_at IS NULL').all().map(row => [row.session_id, row]));
  const connections = sessions.map(session => {
    const row = recorded.get(session.id);
    return { sessionId: session.id, account: names.get(session.userId) ?? null, desktopId: session.desktopId, desktopName: row?.desktop_name ?? session.desktopId, mode: 'stream',
      since: iso(row?.started_at ?? session.createdAt ?? null), ready: session.streamReady === true,
      platform: row?.platform ?? null, version: row?.client_version ?? null, address: row?.address ?? null, activeMs: null };
  }).sort((a, b) => Date.parse(a.since ?? 0) - Date.parse(b.since ?? 0));
  const perToken = new Map(); for (const session of sessions) perToken.set(session.tokenHash, (perToken.get(session.tokenHash) ?? 0) + 1);
  // Nothing about a person's input is read here (input time is not measured).
  const logins = db.prepare(`SELECT s.token_hash,s.created_at,s.issuer,u.username,l.platform,l.client_version,l.address
    FROM sessions s JOIN users u ON u.id=s.user_id LEFT JOIN usage_logins l ON l.token_hash=s.token_hash
    WHERE s.purpose='client' AND s.replaced_at IS NULL AND s.expires_at>? AND u.disabled=0 ORDER BY s.created_at DESC LIMIT 500`).all(now)
    .map(row => ({ account: row.username, since: iso(row.created_at), platform: row.platform ?? ({ 'windows-pc-v1': 'windows', 'browser-console': 'web', legacy: 'android' })[row.issuer] ?? null,
      version: row.client_version ?? null, address: row.address ?? null, connections: perToken.get(row.token_hash) ?? 0 }));
  const count = table => Number(db.prepare(`SELECT count(*) n FROM ${table} WHERE started_at>=? AND started_at<?`).get(start, end).n);
  return { protocol: usageProtocol, checkedAt: iso(now), ...zone(now), today: { date: today, logins: count('usage_logins'), connections: count('usage_connections') },
    totals: { logins: sequence(store, 'usage_logins'), connections: sequence(store, 'usage_connections') }, recordingSince: recordingSince(store), retentionDays: usageRetentionDays,
    people: new Set(logins.map(row => row.account)).size, logins, connections };
}

// One connection or sign-in as the console lists it. activeMs is reserved for
// a later client-reported active time; the broker does not know it today.
function eventView(row, names, live, now) {
  const open = row.ended_at === null, running = row.kind === 'connection' ? open && live.has(row.session_id) : open && row.live === 1;
  // An open connection the relay no longer holds ended at its last heartbeat.
  const stale = row.kind === 'connection' && open && !running;
  const end = running ? null : stale ? row.last_seen_at : row.ended_at;
  return { kind: row.kind, account: names.get(row.user_id) ?? null, startedAt: iso(row.started_at), endedAt: iso(end), live: running,
    durationMs: running ? Math.max(0, now - row.started_at) : end === null ? null : Math.max(0, end - row.started_at), estimated: row.estimated === 1 || stale,
    endReason: running ? null : stale ? 'unknown' : row.end_reason ?? null, ...(row.kind === 'connection' ? { desktopId: row.desktop_id, desktopName: row.desktop_name, mode: row.mode, activeMs: null } : {}),
    platform: row.platform ?? null, version: row.client_version ?? null, address: row.address ?? null, source: row.source };
}
// 기록: the sign-ins and connections that started in [from, to], newest first,
// one page at a time; optionally one account's, or one desktop's connections.
export function usageEvents(store, relay, { from, to, account = null, desktopId = null, page = 1 }, now = Date.now()) {
  const span = range(from, to);
  if (!span || !Number.isSafeInteger(page) || page < 1 || page > usageMaxPage) return null;
  if (account === null && desktopId === null && span.days.length > usageAllEventsDays) return null;
  const db = store.db, names = new Map(db.prepare('SELECT id,username FROM users').all().map(row => [row.id, row.username]));
  const user = account === null ? null : db.prepare('SELECT id FROM users WHERE username=?').get(account);
  if (account !== null && !user) return { protocol: usageProtocol, ...zone(now), from, to, page, pageSize: usagePageSize, more: false, counts: { logins: 0, connections: 0 }, accounts: [], events: [] };
  const scope = (user ? ' AND user_id=?' : ''), args = [span.start, span.end, ...(user ? [user.id] : [])];
  const connectionScope = scope + (desktopId !== null ? ' AND desktop_id=?' : ''), connectionArgs = [...args, ...(desktopId !== null ? [desktopId] : [])];
  const parts = [`SELECT 'connection' kind,id,session_id,user_id,started_at,ended_at,last_seen_at,end_reason,estimated,desktop_id,desktop_name,mode,platform,client_version,address,source,0 live
      FROM usage_connections WHERE started_at>=? AND started_at<?${connectionScope}`];
  const partArgs = [...connectionArgs];
  if (desktopId === null) {
    parts.push(`SELECT 'login',id,NULL,user_id,started_at,ended_at,NULL,end_reason,estimated,NULL,NULL,NULL,platform,client_version,address,source,token_hash IS NOT NULL
      FROM usage_logins WHERE started_at>=? AND started_at<?${scope}`);
    partArgs.push(...args);
  }
  const rows = db.prepare(`${parts.join(' UNION ALL ')} ORDER BY started_at DESC,kind,id DESC LIMIT ? OFFSET ?`).all(...partArgs, usagePageSize + 1, (page - 1) * usagePageSize);
  const live = liveIds(relay);
  const grouped = (table, where, values) => db.prepare(`SELECT user_id,count(*) n FROM ${table} WHERE started_at>=? AND started_at<?${where} GROUP BY user_id`).all(...values);
  const accounts = new Map(), add = (id, key, n) => { const entry = accounts.get(id) ?? { account: names.get(id) ?? null, logins: 0, connections: 0 }; entry[key] += Number(n); accounts.set(id, entry); };
  if (desktopId === null) for (const row of grouped('usage_logins', scope, args)) add(row.user_id, 'logins', row.n);
  for (const row of grouped('usage_connections', connectionScope, connectionArgs)) add(row.user_id, 'connections', row.n);
  const list = [...accounts.values()].sort((a, b) => (b.logins + b.connections) - (a.logins + a.connections) || String(a.account).localeCompare(String(b.account), 'ko'));
  return { protocol: usageProtocol, ...zone(now), from, to, page, pageSize: usagePageSize, more: rows.length > usagePageSize,
    counts: { logins: list.reduce((sum, entry) => sum + entry.logins, 0), connections: list.reduce((sum, entry) => sum + entry.connections, 0) }, accounts: list,
    events: rows.slice(0, usagePageSize).map(row => eventView(row, names, live, now)) };
}

// 사용자별 · 데스크톱별: connected time, connections and sign-ins of each
// account and each desktop over [from, to], with one bucket per day. A
// connection that crosses midnight is split between its days; one still
// running counts up to now. Closed connections that began more than 31 days
// before the range are not looked for (an index range instead of a full scan).
// A year of a busy install is a few hundred thousand rows: they are read in
// chunks by (started_at, id) with a pause between chunks, so the relay's
// packets and other requests are never held for more than one chunk.
export async function usageSummary(store, relay, { from, to }, now = Date.now(), { chunk = usageSummaryChunk, pause = () => new Promise(resolve => setImmediate(resolve)) } = {}) {
  const span = range(from, to);
  if (!span) return null;
  const db = store.db, { days } = span, live = liveIds(relay);
  const names = new Map(db.prepare('SELECT id,username FROM users').all().map(row => [row.id, row.username]));
  const blank = () => ({ logins: 0, connections: 0, connectedMs: 0, firstAt: null, lastAt: null, daily: new Array(days.length).fill(0), dailyConnections: new Array(days.length).fill(0) });
  const users = new Map(), desktops = new Map(), totals = blank();
  const entry = (map, key, extra) => { let value = map.get(key); if (!value) map.set(key, value = { ...extra(), ...blank() }); return value; };
  let read = 0, truncated = false;
  const columns = 'id,session_id,user_id,desktop_id,desktop_name,started_at,ended_at,last_seen_at', reach = span.start - 31 * 86400000;
  const add = row => {
    const end = row.ended_at ?? (live.has(row.session_id) ? now : row.last_seen_at);
    const begin = Math.max(row.started_at, span.start), until = Math.min(end, span.end);
    const started = row.started_at >= span.start && row.started_at < span.end;
    if (!(until > begin) && !started) return;
    const user = entry(users, row.user_id, () => ({ account: names.get(row.user_id) ?? null })), desktop = entry(desktops, row.desktop_id, () => ({ desktopId: row.desktop_id, desktopName: row.desktop_name, users: new Map() }));
    desktop.desktopName = row.desktop_name;
    const share = entry(desktop.users, row.user_id, () => ({ account: names.get(row.user_id) ?? null }));
    const targets = [user, desktop, share, totals];
    // The first day this connection touches, then day by day.
    let index = Math.max(0, Math.min(days.length - 1, Math.floor((begin - span.start) / 86400000)));
    while (index > 0 && days[index].start > begin) index--;
    while (index < days.length - 1 && days[index].end <= begin) index++;
    if (started) for (const target of targets) { target.connections++; target.dailyConnections[index]++; }
    for (let at = begin; at < until && index < days.length; index++) {
      const piece = Math.min(until, days[index].end) - at;
      if (piece > 0) for (const target of targets) { target.connectedMs += piece; target.daily[index] += piece; }
      at = days[index].end;
    }
    for (const target of targets) {
      target.firstAt = target.firstAt === null ? begin : Math.min(target.firstAt, begin);
      target.lastAt = target.lastAt === null ? Math.max(begin, until) : Math.max(target.lastAt, begin, until);
    }
  };
  // Connections still open from long before the range (few), then the range in chunks.
  for (const row of db.prepare(`SELECT ${columns} FROM usage_connections WHERE ended_at IS NULL AND started_at<? LIMIT 2000`).all(reach)) add(row);
  const next = db.prepare(`SELECT ${columns} FROM usage_connections WHERE (started_at,id)>(?,?) AND started_at<? ORDER BY started_at,id LIMIT ?`);
  for (let after = reach, id = -1; ;) {
    const rows = next.all(after, id, span.end, chunk);
    for (const row of rows) if (row.ended_at === null || row.ended_at > span.start) add(row);
    read += rows.length;
    if (rows.length < chunk) break;
    if (read >= usageSummaryRows) { truncated = true; break; }
    after = rows.at(-1).started_at; id = rows.at(-1).id;
    await pause();
  }
  for (const row of db.prepare('SELECT user_id,count(*) n,min(started_at) first,max(started_at) last FROM usage_logins WHERE started_at>=? AND started_at<? GROUP BY user_id').all(span.start, span.end)) {
    const user = entry(users, row.user_id, () => ({ account: names.get(row.user_id) ?? null }));
    user.logins = Number(row.n); totals.logins += Number(row.n);
    user.firstAt = user.firstAt === null ? row.first : Math.min(user.firstAt, row.first); user.lastAt = user.lastAt === null ? row.last : Math.max(user.lastAt, row.last);
  }
  const view = ({ firstAt, lastAt, ...value }) => ({ ...value, firstAt: iso(firstAt), lastAt: iso(lastAt), activeMs: null });
  const byTime = (a, b) => b.connectedMs - a.connectedMs || b.connections - a.connections;
  return { protocol: usageProtocol, ...zone(now), from, to, days: days.map(day => day.key), truncated, recordingSince: recordingSince(store), retentionDays: usageRetentionDays,
    totals: view(totals),
    users: [...users.values()].map(view).sort((a, b) => byTime(a, b) || b.logins - a.logins || String(a.account).localeCompare(String(b.account), 'ko')),
    desktops: [...desktops.values()].map(({ users: shares, logins, ...value }) => ({ ...view(value), users: [...shares.values()].map(({ account, connections, connectedMs }) => ({ account, connections, connectedMs })).sort(byTime) })).sort(byTime) };
}

// The same range asked again within a few seconds (the console polls, several
// administrators look) gets the answer already being computed.
const summaryCache = new WeakMap();
export function usageSummaryShared(store, relay, { from, to }, ttlMs = 10000) {
  let cache = summaryCache.get(store);
  if (!cache) summaryCache.set(store, cache = new Map());
  const key = from + '/' + to, at = Date.now(), held = cache.get(key);
  if (held && at - held.at < ttlMs) return held.value;
  const value = usageSummary(store, relay, { from, to });
  cache.set(key, { at, value });
  for (const [name, entry] of cache) if (cache.size > 16 || at - entry.at >= ttlMs) { if (name !== key) cache.delete(name); }
  value.catch(() => { if (cache.get(key)?.value === value) cache.delete(key); });
  return value;
}
