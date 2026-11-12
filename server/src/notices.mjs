import { createHash, randomBytes } from 'node:crypto';
import { HttpError, fields } from './errors.mjs';
import { usageLive } from './usage.mjs';

// 공지 보내기 (notice-v1): an administrator sends a short plain-text notice to
// the people who are signed in or connected.
//
// notices            one row per notice: who sent it, to whom, when it stops
//                    showing, and three counters (apps that received it,
//                    people who received it, people who saw it).
// notice_recipients  one row per account that was addressed or has received
//                    it: when it was first delivered and when it was seen.
// notice_deliveries  one row per app (sign-in) that fetched it, so one app is
//                    counted once however often it asks. `app` is a hash of the
//                    sign-in's token hash: it cannot be used as a token.
//
// All three are ordinary rowid tables (a backup file is refused if it holds a
// known table of another kind, backup-database.mjs) and additive
// (CREATE ... IF NOT EXISTS, no schema version change):
// an older broker ignores them. An account's rows go with the account; the
// counters on the notice stay.
//
// Who gets it
// - all:     every account that reads its notices before the notice expires
//            (also one that signs in later);
// - user:    one account, wherever and whenever it signs in before the expiry;
// - desktop: the accounts connected (stream or Console) to those desktops at
//            the moment of sending.
// How long it shows
// - 일반 (general): 24 hours;
// - 점검 예고 (maintenance): until the stated time (at most 30 days ahead);
// - 먼저 알리기 (an operation's own notice): until 10 minutes after the wait.
// A withdrawn notice stops showing at once and stays in the history.
// 지우기: a person removes a notice from
// their own list. It is kept per account (notice_recipients.removed_at), so it
// stays gone on their other devices and after the next sign-in. It hides the
// notice from that account only: the notice stays in 보낸 공지, and removing
// counts as seen. The list answer says `features.remove`, which an older
// broker's answer lacks; a client offers 지우기 only when it is there.
//
// Delivery: clients that ask for it get a contentless `notices` push on the
// channel they already hold (/api/v1/events with X-ToTB-Live: notices; a
// stream's control socket opened with the same header) and then read
// GET /api/v1/notices, which is also what a client reads after signing in.
// A released client asks for neither and never calls the route.
// Text is stored and returned as text. Nothing here parses, links or runs it.
export const noticeProtocol = 'notice-v1';
export const noticeTitleMax = 60, noticeBodyMax = 500, noticeBodyLines = 12;
export const noticeGeneralLifetimeMs = 24 * 3600000, noticeMaintenanceAheadMs = 30 * 86400000;
export const noticeOperationGraceMs = 10 * 60000, noticeOperationWaitMaxMs = 3600000;
export const noticeDesktopTargets = 200;
// What a client is given at most, and what the console's history lists.
export const noticeClientMax = 20, noticeHistoryMax = 50;
// History: 180 days like the administration record, and at most this many rows.
export const noticeRetentionMs = 180 * 86400000, noticeRowsKept = 2000;
// An administrator sends at most this many a minute; all administrators together three times that.
export const noticeSendsPerMinute = 10, noticeClientReadsPerMinute = 120;

export const noticeSchema = `
CREATE TABLE IF NOT EXISTS notices (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  public_id TEXT NOT NULL UNIQUE,
  kind TEXT NOT NULL CHECK(kind IN ('general','maintenance')),
  title TEXT NOT NULL CHECK(length(title) BETWEEN 1 AND 120),
  body TEXT NOT NULL CHECK(length(body)<=1200),
  target_kind TEXT NOT NULL CHECK(target_kind IN ('all','user','desktop')),
  target_label TEXT NOT NULL CHECK(length(target_label)<=200),
  scheduled_at INTEGER,
  created_at INTEGER NOT NULL, expires_at INTEGER NOT NULL, withdrawn_at INTEGER,
  actor TEXT NOT NULL, actor_name TEXT NOT NULL,
  source TEXT NOT NULL DEFAULT 'manual' CHECK(source IN ('manual','operation')),
  addressed INTEGER NOT NULL DEFAULT 0,
  apps INTEGER NOT NULL DEFAULT 0, people INTEGER NOT NULL DEFAULT 0, seen INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS notices_expires ON notices(expires_at);
CREATE INDEX IF NOT EXISTS notices_created ON notices(created_at);
CREATE TABLE IF NOT EXISTS notice_recipients (
  notice_id INTEGER NOT NULL REFERENCES notices(id) ON DELETE CASCADE,
  user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  delivered_at INTEGER, seen_at INTEGER,
  PRIMARY KEY(notice_id,user_id)
);
CREATE INDEX IF NOT EXISTS notice_recipients_user ON notice_recipients(user_id);
CREATE TABLE IF NOT EXISTS notice_deliveries (
  notice_id INTEGER NOT NULL REFERENCES notices(id) ON DELETE CASCADE,
  app TEXT NOT NULL,
  user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  delivered_at INTEGER NOT NULL,
  PRIMARY KEY(notice_id,app)
);
`;
export function installNotices(store) {
  store.db.exec(noticeSchema);
  // 지우기: additive like the tables themselves (no schema version change). An
  // older broker names its columns when it writes, so it runs on this too.
  if (!store.db.prepare("SELECT 1 FROM pragma_table_info('notice_recipients') WHERE name='removed_at'").get()) store.db.exec('ALTER TABLE notice_recipients ADD COLUMN removed_at INTEGER');
}

// Retention steps (retention.mjs): history older than noticeRetentionMs that no
// longer shows anywhere, then the oldest beyond noticeRowsKept. The recipient
// and delivery rows go with their notice (ON DELETE CASCADE).
export const noticeRetentionRules = batch => [
  (store, now) => store.db.prepare('DELETE FROM notices WHERE id IN (SELECT id FROM notices WHERE created_at<? AND (expires_at<=? OR withdrawn_at IS NOT NULL) LIMIT ?)').run(now - noticeRetentionMs, now, batch).changes,
  (store, now) => {
    const { last } = store.db.prepare('SELECT max(id) last FROM notices').get();
    return last > noticeRowsKept ? store.db.prepare('DELETE FROM notices WHERE id IN (SELECT id FROM notices WHERE id<=? AND (expires_at<=? OR withdrawn_at IS NOT NULL) ORDER BY id LIMIT ?)').run(last - noticeRowsKept, now, batch).changes : 0;
  },
  // Who received a notice matters while it shows and for its counters, which
  // are on the notice itself: 30 days after its end the per-account rows go.
  (store, now) => store.db.prepare('DELETE FROM notice_deliveries WHERE notice_id IN (SELECT id FROM notices WHERE max(expires_at,COALESCE(withdrawn_at,0))<? AND EXISTS (SELECT 1 FROM notice_deliveries d WHERE d.notice_id=notices.id) LIMIT ?)').run(now - 30 * 86400000, Math.max(1, Math.floor(batch / 10))).changes,
  (store, now) => store.db.prepare('DELETE FROM notice_recipients WHERE notice_id IN (SELECT id FROM notices WHERE max(expires_at,COALESCE(withdrawn_at,0))<? AND EXISTS (SELECT 1 FROM notice_recipients r WHERE r.notice_id=notices.id) LIMIT ?)').run(now - 30 * 86400000, Math.max(1, Math.floor(batch / 10))).changes,
];

const iso = value => Number.isFinite(value) ? new Date(value).toISOString() : null;
const publicIdPattern = /^[a-f0-9]{32}$/;
// Line feeds are the only control characters a notice keeps; a direction
// override could make text read as something it is not.
const forbidden = /[\u0000-\u0009\u000b-\u001f\u007f-\u009f‎‏‪-‮⁦-⁩﻿]/;
const count = text => [...text].length;
export function noticeText(input) {
  if (typeof input?.title !== 'string' || (input.body !== undefined && typeof input.body !== 'string')) throw new HttpError(400, 'invalid_notice_text');
  const title = input.title.replace(/\s+/g, ' ').trim();
  const body = (input.body ?? '').replace(/\r\n?/g, '\n').split('\n').map(line => line.replace(/[ \t]+$/g, '')).join('\n').replace(/\n{3,}/g, '\n\n').trim();
  if (!title || count(title) > noticeTitleMax || forbidden.test(title)) throw new HttpError(400, 'invalid_notice_title');
  if (count(body) > noticeBodyMax || body.split('\n').length > noticeBodyLines || forbidden.test(body)) throw new HttpError(400, 'invalid_notice_body');
  return { title, body };
}
const appKey = hash => createHash('sha256').update('notice-app:' + hash).digest('hex').slice(0, 32);

export class NoticeService {
  // changed(userIds|null): tells the live channels that those accounts (null: everyone) have something to read.
  constructor({ store, relay = null, changed = () => {}, now = Date.now, sendsPerMinute = noticeSendsPerMinute, readsPerMinute = noticeClientReadsPerMinute }) {
    Object.assign(this, { store, relay, changed, now, sendsPerMinute, readsPerMinute, sends: new Map(), reads: new Map() });
  }
  // A small fixed-window limiter of its own: http.mjs imports this module.
  take(table, key, limit, at) {
    let entry = table.get(key);
    if (!entry || entry.until <= at) {
      if (table.size > 5000) for (const [name, value] of table) if (value.until <= at) table.delete(name);
      entry = { count: 0, until: at + 60000 }; table.set(key, entry);
    }
    return ++entry.count <= limit;
  }
  // Accounts connected to these desktops now: streams the relay holds and open Consoles.
  connectedUsers(desktopIds) {
    const ids = new Set(desktopIds), users = new Set();
    for (const session of this.relay?.sessions?.values() ?? []) if (ids.has(session.desktopId) && (session.ws || session.detached) && typeof session.userId === 'string') users.add(session.userId);
    for (const grant of this.relay?.console?.grants?.values() ?? []) if (!grant.closed && ids.has(grant.desktopId) && typeof grant.userId === 'string') users.add(grant.userId);
    return users;
  }
  // Who is signed in or connected right now: the 모니터링 page's live data
  // without its clock fields, so an unchanged audience is an unchanged answer.
  audience(at = this.now()) {
    const live = usageLive(this.store, this.relay, at);
    return { people: live.people, apps: live.logins.length,
      logins: live.logins.map(row => ({ account: row.account, platform: row.platform, version: row.version, since: row.since, connections: row.connections })),
      connections: live.connections.map(row => ({ account: row.account, desktopId: row.desktopId, desktopName: row.desktopName, mode: row.mode, since: row.since })) };
  }
  // lang: the sending console's language (Accept-Language); it names a group of desktops in the record.
  resolveTarget(target, at, lang = 'ko') {
    const db = this.store.db;
    if (!target || typeof target !== 'object' || Array.isArray(target)) throw new HttpError(400, 'invalid_notice_target');
    if (target.kind === 'all') {
      fields(target, ['kind']);
      const signedIn = db.prepare("SELECT count(DISTINCT s.user_id) n FROM sessions s JOIN users u ON u.id=s.user_id WHERE s.purpose='client' AND s.replaced_at IS NULL AND s.expires_at>? AND u.disabled=0").get(at).n;
      return { kind: 'all', label: '', users: null, addressed: Number(signedIn), token: 'all' };
    }
    if (target.kind === 'user') {
      fields(target, ['kind', 'username']);
      if (typeof target.username !== 'string' || !target.username || target.username.length > 128) throw new HttpError(400, 'invalid_notice_target');
      let user; try { user = this.store.findUser(target.username); } catch { throw new HttpError(400, 'invalid_notice_target'); }
      if (!user) throw new HttpError(404, 'user_not_found');
      if (user.disabled) throw new HttpError(409, 'notice_no_recipients');
      return { kind: 'user', label: user.username, users: [user.id], addressed: 1, token: user.id };
    }
    if (target.kind === 'desktop') {
      fields(target, ['kind', 'desktopIds']);
      const ids = target.desktopIds;
      if (!Array.isArray(ids) || !ids.length || ids.length > noticeDesktopTargets || ids.some(id => typeof id !== 'string' || !/^[A-Za-z0-9_-]{1,128}$/.test(id)) || new Set(ids).size !== ids.length) throw new HttpError(400, 'invalid_notice_target');
      const registered = this.store.registeredHostIds();
      if (ids.some(id => !registered.has(id))) throw new HttpError(404, 'desktop_not_found');
      const names = ids.map(id => this.store.desktopName(id) ?? id);
      const enabled = new Set(db.prepare('SELECT id FROM users WHERE disabled=0').all().map(row => row.id));
      const users = [...this.connectedUsers(ids)].filter(id => enabled.has(id));
      if (!users.length) throw new HttpError(409, 'notice_no_recipients');
      return { kind: 'desktop', label: ids.length === 1 ? String(names[0]).slice(0, 200) : lang === 'ko' ? `데스크톱 ${ids.length}대` : `${ids.length} desktops`, users, addressed: users.length, token: ids.length === 1 ? ids[0] : 'desktops' };
    }
    throw new HttpError(400, 'invalid_notice_target');
  }
  // input: { kind, title, body, target, scheduledAt?, operation?: { waitSeconds } }.
  // authorize() is called again right before anything is written.
  send(input, actor, authorize = () => {}, lang = 'ko') {
    const at = this.now();
    fields(input, ['kind', 'title', 'body', 'target', 'scheduledAt', 'operation']);
    const { title, body } = noticeText(input);
    let kind = input.kind, scheduledAt = null, expiresAt, source = 'manual';
    if (input.operation !== undefined) {
      // 먼저 알리기: the operation's own notice. It names the moment the operation starts.
      if (!input.operation || typeof input.operation !== 'object' || Array.isArray(input.operation)) throw new HttpError(400, 'invalid_notice_schedule');
      fields(input.operation, ['waitSeconds']);
      const wait = input.operation.waitSeconds;
      if (!Number.isSafeInteger(wait) || wait < 0 || wait * 1000 > noticeOperationWaitMaxMs || input.scheduledAt !== undefined || (kind !== undefined && kind !== 'maintenance')) throw new HttpError(400, 'invalid_notice_schedule');
      kind = 'maintenance'; source = 'operation'; scheduledAt = at + wait * 1000; expiresAt = scheduledAt + noticeOperationGraceMs;
    } else if (kind === 'general') {
      if (input.scheduledAt !== undefined && input.scheduledAt !== null) throw new HttpError(400, 'invalid_notice_schedule');
      expiresAt = at + noticeGeneralLifetimeMs;
    } else if (kind === 'maintenance') {
      scheduledAt = typeof input.scheduledAt === 'string' && input.scheduledAt.length <= 40 ? Date.parse(input.scheduledAt) : NaN;
      if (!Number.isFinite(scheduledAt) || scheduledAt <= at || scheduledAt > at + noticeMaintenanceAheadMs) throw new HttpError(400, 'invalid_notice_schedule');
      expiresAt = scheduledAt;
    } else throw new HttpError(400, 'invalid_notice_kind');
    const target = this.resolveTarget(input.target, at, lang);
    // Counted once the request is one that could be sent: a mistyped form does not use the budget up.
    if (!this.take(this.sends, actor.id, this.sendsPerMinute, at) || !this.take(this.sends, '*', this.sendsPerMinute * 3, at)) throw new HttpError(429, 'too_many_notices');
    const publicId = randomBytes(16).toString('hex');
    this.store.transaction(() => {
      authorize();
      const id = this.store.db.prepare(`INSERT INTO notices(public_id,kind,title,body,target_kind,target_label,scheduled_at,created_at,expires_at,actor,actor_name,source,addressed)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)`).run(publicId, kind, title, body, target.kind, target.label, scheduledAt, at, expiresAt, actor.id, String(actor.username).slice(0, 128), source, target.addressed).lastInsertRowid;
      const add = this.store.db.prepare('INSERT OR IGNORE INTO notice_recipients(notice_id,user_id) VALUES(?,?)');
      for (const userId of target.users ?? []) add.run(id, userId);
      // target:kind:addressed (labels.js notice_sent). The text is not copied into the administration record.
      this.store.audit('notice_sent', actor.id, `${target.token}:${kind}:${target.addressed}`, at);
    });
    this.changed(target.users);
    return { notice: this.adminView(this.store.db.prepare('SELECT * FROM notices WHERE public_id=?').get(publicId), at) };
  }
  withdraw(publicId, actor, authorize = () => {}) {
    const at = this.now();
    if (!publicIdPattern.test(publicId ?? '')) throw new HttpError(404, 'notice_not_found');
    const row = this.store.db.prepare('SELECT * FROM notices WHERE public_id=?').get(publicId);
    if (!row) throw new HttpError(404, 'notice_not_found');
    if (row.withdrawn_at !== null || row.expires_at <= at) throw new HttpError(409, 'notice_already_ended');
    const users = row.target_kind === 'all' ? null : this.store.db.prepare('SELECT user_id FROM notice_recipients WHERE notice_id=?').all(row.id).map(entry => entry.user_id);
    this.store.transaction(() => {
      authorize();
      this.store.db.prepare('UPDATE notices SET withdrawn_at=? WHERE id=? AND withdrawn_at IS NULL').run(at, row.id);
      this.store.audit('notice_withdrawn', actor.id, `${row.target_kind}:${row.kind}`, at);
    });
    this.changed(users);
    return { notice: this.adminView(this.store.db.prepare('SELECT * FROM notices WHERE id=?').get(row.id), at) };
  }
  adminView(row, at) {
    return { id: row.public_id, kind: row.kind, title: row.title, body: row.body, target: { kind: row.target_kind, label: row.target_label }, source: row.source,
      sentAt: iso(row.created_at), expiresAt: iso(row.expires_at), scheduledAt: iso(row.scheduled_at), withdrawnAt: iso(row.withdrawn_at), by: row.actor_name,
      addressed: row.addressed, apps: row.apps, people: row.people, seen: row.seen, active: row.withdrawn_at === null && row.expires_at > at };
  }
  // The console's 공지 page: the audience now, the limits it shows, and the history, newest first.
  adminList(at = this.now()) {
    const rows = this.store.db.prepare('SELECT * FROM notices ORDER BY id DESC LIMIT ?').all(noticeHistoryMax);
    return { protocol: noticeProtocol, limits: { title: noticeTitleMax, body: noticeBodyMax, bodyLines: noticeBodyLines, generalHours: noticeGeneralLifetimeMs / 3600000, maintenanceDays: noticeMaintenanceAheadMs / 86400000, sendsPerMinute: this.sendsPerMinute, retentionDays: noticeRetentionMs / 86400000 },
      audience: this.audience(at), notices: rows.map(row => this.adminView(row, at)) };
  }
  // What one account's app shows now. Reading is the delivery: the app is
  // counted once per notice, the account once.
  clientList(user, hash, at = this.now()) {
    if (!this.take(this.reads, user.id, this.readsPerMinute, at)) throw new HttpError(429, 'too_many_notice_requests');
    const db = this.store.db, app = appKey(hash);
    const rows = db.prepare(`SELECT n.id,n.public_id,n.kind,n.title,n.body,n.scheduled_at,n.created_at,n.expires_at,r.delivered_at,r.seen_at,
        (SELECT 1 FROM notice_deliveries d WHERE d.notice_id=n.id AND d.app=?) AS known
      FROM notices n LEFT JOIN notice_recipients r ON r.notice_id=n.id AND r.user_id=?
      WHERE n.expires_at>? AND n.withdrawn_at IS NULL AND (n.target_kind='all' OR r.user_id IS NOT NULL) AND r.removed_at IS NULL ORDER BY n.id DESC LIMIT ?`).all(app, user.id, at, noticeClientMax);
    const fresh = rows.filter(row => row.known !== 1);
    if (fresh.length) this.store.transaction(() => {
      for (const row of fresh) {
        if (!db.prepare('INSERT OR IGNORE INTO notice_deliveries(notice_id,app,user_id,delivered_at) VALUES(?,?,?,?)').run(row.id, app, user.id, at).changes) continue;
        db.prepare('INSERT OR IGNORE INTO notice_recipients(notice_id,user_id) VALUES(?,?)').run(row.id, user.id);
        const first = db.prepare('UPDATE notice_recipients SET delivered_at=? WHERE notice_id=? AND user_id=? AND delivered_at IS NULL').run(at, row.id, user.id).changes;
        db.prepare('UPDATE notices SET apps=apps+1,people=people+? WHERE id=?').run(first ? 1 : 0, row.id);
      }
    });
    // features: what this broker adds to notice-v1 (a client that does not find a name runs without it).
    return { protocol: noticeProtocol, serverTime: iso(at), features: { remove: 1 }, notices: rows.map(row => ({ id: row.public_id, kind: row.kind, title: row.title, body: row.body,
      sentAt: iso(row.created_at), expiresAt: iso(row.expires_at), scheduledAt: iso(row.scheduled_at), seen: row.seen_at !== null })) };
  }
  // The person closed the notice by hand or opened the list: seen, once per account.
  markSeen(user, input, at = this.now()) {
    if (!this.take(this.reads, user.id, this.readsPerMinute, at)) throw new HttpError(429, 'too_many_notice_requests');
    fields(input, ['ids']);
    const ids = input.ids;
    if (!Array.isArray(ids) || !ids.length || ids.length > noticeClientMax || ids.some(id => !publicIdPattern.test(id ?? '')) || new Set(ids).size !== ids.length) throw new HttpError(400, 'invalid_notice_ids');
    const db = this.store.db;
    let marked = 0;
    this.store.transaction(() => {
      for (const publicId of ids) {
        // Only a notice this account was given can be marked: one it could not read stays unknown to it.
        const row = db.prepare('SELECT n.id FROM notices n JOIN notice_recipients r ON r.notice_id=n.id AND r.user_id=? WHERE n.public_id=? AND r.delivered_at IS NOT NULL').get(user.id, publicId);
        if (!row || !db.prepare('UPDATE notice_recipients SET seen_at=? WHERE notice_id=? AND user_id=? AND seen_at IS NULL').run(at, row.id, user.id).changes) continue;
        db.prepare('UPDATE notices SET seen=seen+1 WHERE id=?').run(row.id); marked++;
      }
    });
    // The account's other apps drop the notice from their unread count.
    if (marked) this.changed([user.id]);
    return { ok: true, marked };
  }
  // 지우기: the person removed these from their list. Only a notice this account
  // was given can be removed (another account's, an unknown one: nothing, and
  // the same answer). One that has ended meanwhile is taken too: it shows
  // nowhere anyway. Removing is seeing, once per account.
  remove(user, input, at = this.now()) {
    if (!this.take(this.reads, user.id, this.readsPerMinute, at)) throw new HttpError(429, 'too_many_notice_requests');
    fields(input, ['ids']);
    const ids = input.ids;
    if (!Array.isArray(ids) || !ids.length || ids.length > noticeClientMax || ids.some(id => !publicIdPattern.test(id ?? '')) || new Set(ids).size !== ids.length) throw new HttpError(400, 'invalid_notice_ids');
    const db = this.store.db;
    let removed = 0;
    this.store.transaction(() => {
      for (const publicId of ids) {
        const row = db.prepare('SELECT n.id,r.seen_at FROM notices n JOIN notice_recipients r ON r.notice_id=n.id AND r.user_id=? WHERE n.public_id=? AND r.delivered_at IS NOT NULL AND r.removed_at IS NULL').get(user.id, publicId);
        if (!row) continue;
        db.prepare('UPDATE notice_recipients SET removed_at=?,seen_at=COALESCE(seen_at,?) WHERE notice_id=? AND user_id=?').run(at, at, row.id, user.id);
        if (row.seen_at === null) db.prepare('UPDATE notices SET seen=seen+1 WHERE id=?').run(row.id);
        removed++;
      }
    });
    // The account's other apps drop it from their list.
    if (removed) this.changed([user.id]);
    return { ok: true, removed };
  }
}
