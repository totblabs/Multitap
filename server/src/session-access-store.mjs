// Schema18 and the synchronous Store authority used by session-access-v1.
// Token hashes are retained for bounded termination notices; tokens and
// passwords are never stored here.
import { sessionInputPersistenceDeferred } from './session-access.mjs';
export const sessionAccessSchema = `
CREATE TABLE session_activity (
  token_hash TEXT PRIMARY KEY REFERENCES sessions(token_hash) ON DELETE CASCADE,
  tracking_started_at INTEGER NOT NULL CHECK(tracking_started_at>=0),
  idle_enabled INTEGER NOT NULL DEFAULT 1 CHECK(idle_enabled IN (0,1)),
  last_input_at INTEGER CHECK(last_input_at IS NULL OR last_input_at>=tracking_started_at)
);
CREATE TABLE session_end_reasons (
  token_hash TEXT PRIMARY KEY,
  user_id TEXT NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  reason TEXT NOT NULL CHECK(reason IN ('pc_login_replaced','session_idle_timeout')),
  ended_at INTEGER NOT NULL CHECK(ended_at>=0),
  retain_until INTEGER NOT NULL CHECK(retain_until>ended_at)
);
CREATE INDEX session_end_reasons_retention ON session_end_reasons(retain_until);
CREATE INDEX session_end_reasons_account ON session_end_reasons(user_id,ended_at);
`;

const validHash = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
const retainMs = 7 * 86400000;
const sessionRows = (store, userId, now) => store.db.prepare(`SELECT token_hash AS tokenHash,user_id AS userId,purpose,issuer,created_at AS createdAt
  FROM sessions WHERE user_id=? AND purpose='client' AND replaced_at IS NULL AND expires_at>? ORDER BY token_hash`).all(userId, now);

export function createSessionAccessStore(store, { now = Date.now } = {}) {
  const proofs = new WeakMap();
  const atomic = action => store.db.isTransaction === true ? action() : store.transaction(action);
  const account = id => store.db.prepare('SELECT id,password_hash,disabled,must_change_credentials,authorization_revision FROM users WHERE id=?').get(id) ?? null;
  function initializeTracking(at) {
    // This is the time tracking starts, not an assertion that the user typed.
    store.db.prepare(`INSERT OR IGNORE INTO session_activity(token_hash,tracking_started_at,last_input_at)
      SELECT s.token_hash,?,NULL FROM sessions s WHERE s.purpose='client' AND s.replaced_at IS NULL AND s.created_at<=? AND s.expires_at>?`).run(at, at, at);
  }
  function currentClient(hash, at) {
    let row = store.db.prepare(`SELECT s.token_hash AS tokenHash,s.user_id AS userId,s.purpose,s.created_at AS createdAt,s.expires_at AS expiresAt,
      a.tracking_started_at AS activityStartedAt,a.last_input_at AS lastInputAt,a.idle_enabled AS idleEnabled
      FROM sessions s JOIN users u ON u.id=s.user_id LEFT JOIN session_activity a ON a.token_hash=s.token_hash
      WHERE s.token_hash=? AND s.purpose='client' AND s.replaced_at IS NULL AND s.created_at<=? AND s.expires_at>? AND u.disabled=0 AND u.must_change_credentials=0`).get(hash, at, at);
    if (row && row.activityStartedAt === null) {
      store.db.prepare('INSERT OR IGNORE INTO session_activity(token_hash,tracking_started_at,last_input_at) VALUES(?,?,NULL)').run(hash, at);
      row = { ...row, activityStartedAt: at, lastInputAt: null, idleEnabled: 1 };
    }
    return row ? { ...row, idleEnabled: row.idleEnabled === 1 } : null;
  }
  function endReason(hash, userId, reason, at) {
    if (!['pc_login_replaced','session_idle_timeout'].includes(reason)) throw Error('invalid_session_end_reason');
    store.db.prepare(`INSERT INTO session_end_reasons(token_hash,user_id,reason,ended_at,retain_until) VALUES(?,?,?,?,?)
      ON CONFLICT(token_hash) DO UPDATE SET user_id=excluded.user_id,reason=excluded.reason,ended_at=excluded.ended_at,retain_until=excluded.retain_until`).run(hash, userId, reason, at, at + retainMs);
  }
  function prune(at) {
    store.db.prepare('DELETE FROM session_end_reasons WHERE retain_until<=?').run(at);
    store.db.prepare('DELETE FROM session_end_reasons WHERE token_hash IN (SELECT token_hash FROM session_end_reasons ORDER BY ended_at DESC,token_hash LIMIT -1 OFFSET 4096)').run();
  }
  const authority = {
    account,
    clientSessions: (id, at) => sessionRows(store, id, at),
    commitLogin(binding) {
      return atomic(() => {
        const next = binding.newSession, at = next.createdAt;
        if (!validHash(next.tokenHash) || next.purpose !== 'client' || !['legacy','windows-pc-v1','browser-console'].includes(next.issuer)) throw Error('invalid_session_policy');
        binding.validateCurrent({ account: account(binding.accountId), currentSessions: sessionRows(store, binding.accountId, at),
          totalSessionCount: store.db.prepare('SELECT count(*) AS n FROM sessions WHERE user_id=? AND replaced_at IS NULL AND expires_at>?').get(binding.accountId, at).n });
        // No asynchronous work separates admission, revocation and insertion.
        for (const previous of binding.expectedSessions) {
          const removed = store.db.prepare("DELETE FROM sessions WHERE token_hash=? AND user_id=? AND purpose='client' AND replaced_at IS NULL").run(previous.tokenHash, binding.accountId).changes;
          if (removed !== 1) throw Error('session_access_commit_unverified');
          endReason(previous.tokenHash, binding.accountId, binding.reason, at);
        }
        // Expired historical PC rows still occupy its partial unique index.
        store.db.prepare('DELETE FROM sessions WHERE user_id=? AND expires_at<=?').run(binding.accountId, at);
        store.db.prepare('INSERT INTO sessions(token_hash,user_id,created_at,expires_at,purpose,issuer) VALUES(?,?,?,?,?,?)').run(next.tokenHash, binding.accountId, at, next.expiresAt, 'client', next.issuer);
        store.db.prepare('INSERT INTO session_activity(token_hash,tracking_started_at,last_input_at) VALUES(?,?,NULL)').run(next.tokenHash, at);
        if (binding.expectedSessions.length) store.audit('client_login_replaced', binding.accountId, String(binding.expectedSessions.length), at);
        store.audit('login_succeeded', binding.accountId, '', at); prune(at);
        return true;
      });
    },
    currentClient,
    acceptedInput(proof, at) {
      const bound = proof && typeof proof === 'object' ? proofs.get(proof) : null;
      if (!bound) return null;
      proofs.delete(proof);
      const current = currentClient(bound.tokenHash, at);
      return current?.userId === bound.userId ? bound : null;
    },
    persistInputCAS({ tokenHash, userId, createdAt, expectedLastInputAt, lastInputAt, now: at }) {
      // An enclosing MFA/management transaction may still roll back. Avoid
      // issuing a false durable ACK or clearing the RAM dirty flag: defer the
      // write until the next ordinary autocommit authority guard. All fresh
      // session/idle checks remain in force during this transaction.
      if (store.db.isTransaction === true) return sessionInputPersistenceDeferred;
      return store.db.prepare(`UPDATE session_activity SET last_input_at=? WHERE token_hash=? AND last_input_at IS ?
        AND tracking_started_at<=? AND ?<=? AND (last_input_at IS NULL OR last_input_at<=?) AND EXISTS(SELECT 1 FROM sessions s JOIN users u ON u.id=s.user_id
          WHERE s.token_hash=session_activity.token_hash AND s.user_id=? AND s.created_at=? AND s.purpose='client'
          AND s.replaced_at IS NULL AND s.expires_at>? AND u.disabled=0 AND u.must_change_credentials=0)`)
        .run(lastInputAt, tokenHash, expectedLastInputAt, lastInputAt, lastInputAt, at, lastInputAt, userId, createdAt, at).changes === 1;
    },
    setIdleEnabledCAS({ tokenHash, userId, createdAt, expectedIdleEnabled, expectedLastInputAt, enabled, lastInputAt, now: at, validateCurrent }) {
      return atomic(() => {
        const current = currentClient(tokenHash, at);
        if (!validateCurrent(current) || current?.userId !== userId || current.createdAt !== createdAt || current.idleEnabled !== expectedIdleEnabled || current.lastInputAt !== expectedLastInputAt) return false;
        return store.db.prepare('UPDATE session_activity SET idle_enabled=?,last_input_at=? WHERE token_hash=? AND idle_enabled=? AND last_input_at IS ?')
          .run(enabled ? 1 : 0, lastInputAt, tokenHash, expectedIdleEnabled ? 1 : 0, expectedLastInputAt).changes === 1;
      });
    },
    endIdleCAS({ tokenHash, userId, createdAt, now: at, reason, validateCurrent }) {
      return atomic(() => {
        const current = currentClient(tokenHash, at);
        if (!validateCurrent(current) || current?.userId !== userId || current.createdAt !== createdAt) return false;
        const removed = store.db.prepare("DELETE FROM sessions WHERE token_hash=? AND user_id=? AND created_at=? AND purpose='client'").run(tokenHash, userId, createdAt).changes;
        if (removed !== 1) throw Error('session_access_commit_unverified');
        endReason(tokenHash, userId, reason, at); store.audit('client_idle_logout', userId, '', at); prune(at);
        return true;
      });
    },
    idleCandidates(at, limit) {
      return store.db.prepare(`SELECT s.token_hash FROM sessions s JOIN session_activity a ON a.token_hash=s.token_hash
        JOIN users u ON u.id=s.user_id WHERE s.purpose='client' AND s.replaced_at IS NULL AND s.expires_at>?
        AND u.disabled=0 AND u.must_change_credentials=0 AND a.idle_enabled=1 ORDER BY COALESCE(a.last_input_at,a.tracking_started_at),s.token_hash LIMIT ?`).all(at, limit).map(row => row.token_hash);
    },
  };
  initializeTracking(now());
  return {
    authority,
    mintInputProof(tokenHash, userId, kind) {
      if (!validHash(tokenHash) || !['ui','remote-input'].includes(kind) || currentClient(tokenHash, now())?.userId !== userId) throw Error('session_activity_not_admitted');
      const proof = Object.freeze(Object.create(null)); proofs.set(proof, { tokenHash, userId, kind }); return proof;
    },
  };
}
