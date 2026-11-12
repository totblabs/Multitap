import { createHash, randomBytes as cryptoRandomBytes } from 'node:crypto';
import { pcLoginReplaced } from './client-session-policy.mjs';

export { pcLoginReplaced };
export const sessionAccessProtocol = 'session-access-v1';
// Internal authority signal only. A deferred SQL flush is not a committed
// persistence ACK, and cannot be supplied by peer JSON or telemetry.
export const sessionInputPersistenceDeferred = Symbol('session-input-persistence-deferred');
export const sessionIdleTimeout = 'session_idle_timeout';
export const sessionAccessDefaults = Object.freeze({ idleTimeoutMs: 1800000, warningMs: 60000,
  persistInputIntervalMs: 5000, maximumChallenges: 1024, challengesPerAccount: 4,
  maximumActivityEntries: 4096, sweepBatch: 1024 });
const confirmationTtlMs = 120000, maximumSessions = 5;
const hashPattern = /^[a-f0-9]{64}$/, identityPattern = /^[A-Za-z0-9_-]{1,128}$/;
const issuerPattern = /^[A-Za-z0-9_-]{1,64}$/, challengePattern = /^[A-Za-z0-9_-]{43}$/;
const digest = value => createHash('sha256').update(value).digest('hex');
const integer = (value, minimum = 0, maximum = 8640000000000000) => Number.isSafeInteger(value) && value >= minimum && value <= maximum;
const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
export class SessionAccessError extends Error { constructor(code) { super(code); this.name = 'SessionAccessError'; this.code = code; } }
const fail = code => { throw new SessionAccessError(code); };
function sync(value) { if (value && typeof value.then === 'function') fail('session_access_authority_must_be_synchronous'); return value; }
function accountFingerprint(value) {
  if (!value || typeof value.id !== 'string' || !identityPattern.test(value.id) || typeof value.password_hash !== 'string' ||
      value.password_hash.length < 1 || value.password_hash.length > 4096 || value.disabled !== 0 || value.must_change_credentials !== 0) fail('login_confirmation_stale');
  const authorizationRevision = value.authorization_revision ?? 0;
  if (!integer(authorizationRevision)) fail('login_confirmation_stale');
  return { accountId: value.id, passwordFingerprint: digest(value.password_hash), authorizationRevision };
}
function sessions(value, userId) {
  if (!Array.isArray(value) || value.length > maximumSessions) fail('session_access_unavailable');
  const seen = new Set();
  return value.map(row => {
    if (!row || typeof row.tokenHash !== 'string' || !hashPattern.test(row.tokenHash) || row.userId !== userId || row.purpose !== 'client' ||
        typeof row.issuer !== 'string' || !issuerPattern.test(row.issuer) || !integer(row.createdAt) || seen.has(row.tokenHash)) fail('session_access_unavailable');
    seen.add(row.tokenHash);
    return { tokenHash: row.tokenHash, userId, purpose: 'client', issuer: row.issuer, createdAt: row.createdAt };
  }).sort((a, b) => a.tokenHash.localeCompare(b.tokenHash, 'en'));
}
function newSession(value, now) {
  if (!value || Object.keys(value).sort().join(',') !== 'issuer,tokenHash,ttlMs' || typeof value.tokenHash !== 'string' || !hashPattern.test(value.tokenHash) ||
      typeof value.issuer !== 'string' || !issuerPattern.test(value.issuer) || !integer(value.ttlMs, 1, 7 * 86400000) || !integer(now + value.ttlMs)) fail('invalid_session_access_input');
  return Object.freeze({ tokenHash: value.tokenHash, issuer: value.issuer, purpose: 'client', createdAt: now, expiresAt: now + value.ttlMs });
}

// This module owns no database schema, HTTP route, password verification or
// peer-activity parser. Mandatory synchronous adapters are server authority.
// commitLogin/endIdleCAS MUST call validateCurrent with fresh actual Store rows
// inside the SAME transaction that changes authentication and terminal reasons.
export class SessionAccess {
  #authority; #now; #random; #policy; #challenges = new Map(); #direct = new WeakMap(); #activity = new Map();
  constructor({ authority, now = Date.now, randomBytes = cryptoRandomBytes, policy = {} } = {}) {
    for (const name of ['account', 'clientSessions', 'commitLogin', 'currentClient', 'acceptedInput', 'persistInputCAS', 'endIdleCAS', 'idleCandidates', 'setIdleEnabledCAS']) {
      if (typeof authority?.[name] !== 'function') fail('session_access_authority_required');
    }
    if (typeof now !== 'function' || typeof randomBytes !== 'function' || !policy || typeof policy !== 'object' || Array.isArray(policy) ||
        Object.keys(policy).some(key => !Object.hasOwn(sessionAccessDefaults, key))) fail('invalid_session_access_policy');
    const p = { ...sessionAccessDefaults, ...policy };
    if (!integer(p.idleTimeoutMs, 60000, 7 * 86400000) || !integer(p.warningMs, 0, p.idleTimeoutMs - 1) ||
        !integer(p.persistInputIntervalMs, 1, 30000) || !integer(p.maximumChallenges, 1, 10000) ||
        !integer(p.challengesPerAccount, 1, p.maximumChallenges) || !integer(p.maximumActivityEntries, 1, 100000) || !integer(p.sweepBatch, 1, 10000)) fail('invalid_session_access_policy');
    this.#authority = authority; this.#now = now; this.#random = randomBytes; this.#policy = Object.freeze(p);
  }
  #time() { const now = this.#now(); if (!integer(now)) fail('session_access_clock_unavailable'); return now; }
  #prune(now) { for (const [key, value] of this.#challenges) if (now >= value.expiresAt || now < value.issuedAt) this.#challenges.delete(key); }
  #currentBound(bound, now) {
    const current = accountFingerprint(sync(this.#authority.account(bound.accountId, now)));
    if (!same(current, { accountId: bound.accountId, passwordFingerprint: bound.passwordFingerprint, authorizationRevision: bound.authorizationRevision }) ||
        !same(sessions(sync(this.#authority.clientSessions(bound.accountId, now)), bound.accountId), bound.expectedSessions)) fail('login_confirmation_stale');
  }
  // verifiedAccountSnapshot comes only from Root's successful password/MFA job.
  // It is not a peer JSON object or an assertion of authentication.
  begin(verifiedAccountSnapshot) {
    const now = this.#time(); this.#prune(now);
    const bound = { ...accountFingerprint(verifiedAccountSnapshot), issuedAt: now, expiresAt: now + confirmationTtlMs };
    bound.expectedSessions = sessions(sync(this.#authority.clientSessions(bound.accountId, now)), bound.accountId);
    this.#currentBound(bound, now);
    if (!bound.expectedSessions.length) {
      const admission = Object.freeze(Object.create(null)); this.#direct.set(admission, bound);
      return { kind: 'ready', protocol: sessionAccessProtocol, admission };
    }
    if (this.#challenges.size >= this.#policy.maximumChallenges ||
        [...this.#challenges.values()].filter(value => value.accountId === bound.accountId).length >= this.#policy.challengesPerAccount) fail('login_confirmation_limit');
    for (let i = 0; i < 4; ++i) {
      const bytes = this.#random(32); if (!Buffer.isBuffer(bytes) || bytes.length !== 32) fail('session_access_random_unavailable');
      const challenge = bytes.toString('base64url'), key = digest(challenge);
      if (this.#challenges.has(key)) continue;
      this.#challenges.set(key, bound);
      return { kind: 'confirmation-required', protocol: sessionAccessProtocol, challenge, expiresAt: new Date(bound.expiresAt).toISOString(), sessions: bound.expectedSessions.length };
    }
    fail('session_access_random_unavailable');
  }
  // Only a trusted server callback receives the exact existing-session
  // snapshot. This adds no peer route or authentication/confirmation action.
  describeConfirmation(challenge, describe) {
    if (typeof challenge !== 'string' || !challengePattern.test(challenge) || typeof describe !== 'function') fail('login_confirmation_invalid');
    const now = this.#time(), bound = this.#challenges.get(digest(challenge));
    if (!bound) fail('login_confirmation_invalid');
    if (now < bound.issuedAt || now >= bound.expiresAt) fail('login_confirmation_expired');
    this.#currentBound(bound, now);
    return sync(describe(structuredClone(bound.expectedSessions)));
  }
  #commit(bound, value, now) {
    if (now < bound.issuedAt || now >= bound.expiresAt) fail('login_confirmation_expired');
    this.#currentBound(bound, now);
    const next = newSession(value, now);
    if (bound.expectedSessions.some(row => row.tokenHash === next.tokenHash)) fail('invalid_session_access_input');
    let checked = false;
    const validateCurrent = actual => {
      const current = accountFingerprint(actual?.account);
      if (!same(current, { accountId: bound.accountId, passwordFingerprint: bound.passwordFingerprint, authorizationRevision: bound.authorizationRevision }) ||
          !same(sessions(actual?.currentSessions, bound.accountId), bound.expectedSessions)) fail('login_confirmation_stale');
      if (!integer(actual?.totalSessionCount, bound.expectedSessions.length, maximumSessions) ||
          actual.totalSessionCount - bound.expectedSessions.length + 1 > maximumSessions) fail('active_session_limit');
      checked = true; return true;
    };
    const committed = sync(this.#authority.commitLogin({ accountId: bound.accountId, passwordFingerprint: bound.passwordFingerprint,
      expectedSessions: structuredClone(bound.expectedSessions), newSession: next, reason: pcLoginReplaced, validateCurrent }));
    if (!checked || committed !== true) fail('session_access_commit_unverified');
    for (const row of bound.expectedSessions) this.#activity.delete(row.tokenHash);
    return { created: true, protocol: sessionAccessProtocol, replaced: bound.expectedSessions.length, reason: bound.expectedSessions.length ? pcLoginReplaced : null, expiresAt: new Date(next.expiresAt).toISOString() };
  }
  commitDirect(admission, value) {
    const bound = admission && typeof admission === 'object' ? this.#direct.get(admission) : null;
    if (!bound) fail('login_confirmation_invalid'); this.#direct.delete(admission);
    return this.#commit(bound, value, this.#time());
  }
  confirm(challenge, value) {
    const now = this.#time();
    if (typeof challenge !== 'string' || !challengePattern.test(challenge)) fail('login_confirmation_invalid');
    const key = digest(challenge), bound = this.#challenges.get(key); this.#challenges.delete(key);
    if (!bound) fail('login_confirmation_invalid');
    return this.#commit(bound, value, now);
  }
  cancel(challenge) {
    if (typeof challenge !== 'string' || !challengePattern.test(challenge)) fail('login_confirmation_invalid');
    return { cancelled: this.#challenges.delete(digest(challenge)) };
  }
  #client(hash, now) {
    if (typeof hash !== 'string' || !hashPattern.test(hash)) fail('invalid_session_access_input');
    const row = sync(this.#authority.currentClient(hash, now));
    if (row == null) { this.#activity.delete(hash); return null; }
    if (row.tokenHash !== hash || typeof row.userId !== 'string' || !identityPattern.test(row.userId) || row.purpose !== 'client' ||
        typeof row.idleEnabled !== 'boolean' ||
        !integer(row.createdAt) || !integer(row.expiresAt, row.createdAt + 1) || !integer(row.activityStartedAt, row.createdAt, now) ||
        (row.lastInputAt !== null && !integer(row.lastInputAt, row.activityStartedAt, now))) fail('session_activity_unavailable');
    if (row.expiresAt <= now) { this.#activity.delete(hash); return null; }
    const cached = this.#activity.get(hash);
    if (cached && (cached.userId !== row.userId || cached.createdAt !== row.createdAt)) this.#activity.delete(hash);
    return row;
  }
  #last(row) { return Math.max(row.activityStartedAt, row.lastInputAt ?? row.activityStartedAt, this.#activity.get(row.tokenHash)?.lastInputAt ?? 0); }
  #descriptor(row, now) {
    const deadline = this.#last(row) + this.#policy.idleTimeoutMs;
    return { protocol: sessionAccessProtocol, serverTime: new Date(now).toISOString(), idleDeadline: row.idleEnabled ? new Date(deadline).toISOString() : null,
      warningAt: row.idleEnabled ? new Date(deadline - this.#policy.warningMs).toISOString() : null,
      idleTimeoutMs: this.#policy.idleTimeoutMs, warningMs: this.#policy.warningMs, idleEnabled: row.idleEnabled };
  }
  #persist(row, cached, now) {
    if (!cached.dirty || now - cached.lastPersistAt < this.#policy.persistInputIntervalMs) return;
    const saved = sync(this.#authority.persistInputCAS({ tokenHash: row.tokenHash, userId: row.userId, createdAt: row.createdAt,
      expectedLastInputAt: row.lastInputAt, lastInputAt: cached.lastInputAt, now }));
    // Preserve both dirty state and the last confirmed persistence time. The
    // current/idle guard still uses this exact accepted-input cache normally.
    if (saved === sessionInputPersistenceDeferred) return;
    if (saved !== true) fail('session_activity_unavailable');
    // Throttle by the actual persisted input timestamp, not a delayed flush's
    // wall time: otherwise a first input after a long quiet period could be
    // left far newer than durable state immediately after that stale flush.
    cached.lastPersistAt = cached.lastInputAt; cached.dirty = false;
  }
  #endIdle(row, now) {
    let checked = false, approved = false;
    const validateCurrent = actual => {
      checked = true;
      if (!actual || actual.tokenHash !== row.tokenHash || actual.userId !== row.userId || actual.purpose !== 'client' || actual.idleEnabled !== true || actual.createdAt !== row.createdAt || !integer(actual.expiresAt, row.createdAt + 1) || actual.expiresAt <= now ||
          !integer(actual.activityStartedAt, row.createdAt, now) || (actual.lastInputAt !== null && !integer(actual.lastInputAt, actual.activityStartedAt, now))) return false;
      approved = now >= this.#last(actual) + this.#policy.idleTimeoutMs;
      return approved;
    };
    const ended = sync(this.#authority.endIdleCAS({ tokenHash: row.tokenHash, userId: row.userId, createdAt: row.createdAt, now, reason: sessionIdleTimeout, validateCurrent }));
    if (!checked || typeof ended !== 'boolean' || (ended && !approved)) fail('session_access_commit_unverified');
    if (ended) this.#activity.delete(row.tokenHash);
    return ended;
  }
  recordAcceptedInput(proof) {
    const now = this.#time(), accepted = sync(this.#authority.acceptedInput(proof, now));
    if (!accepted || typeof accepted.tokenHash !== 'string' || !hashPattern.test(accepted.tokenHash) || typeof accepted.userId !== 'string' || !identityPattern.test(accepted.userId) || !['ui', 'remote-input'].includes(accepted.kind)) fail('session_activity_not_admitted');
    let row = this.#client(accepted.tokenHash, now);
    if (!row || row.userId !== accepted.userId) fail('session_expired');
    // Never let resumed activity revive an already idle-expired session.
    if (row.idleEnabled && now >= this.#last(row) + this.#policy.idleTimeoutMs) {
      if (this.#endIdle(row, now)) fail(sessionIdleTimeout);
      row = this.#client(accepted.tokenHash, now);
      if (!row || (row.idleEnabled && now >= this.#last(row) + this.#policy.idleTimeoutMs)) fail('session_activity_unavailable');
    }
    let cached = this.#activity.get(row.tokenHash);
    if (!cached) cached = { userId: row.userId, createdAt: row.createdAt, lastInputAt: now, lastPersistAt: 0, dirty: true, warnedDeadline: null };
    cached.lastInputAt = Math.max(cached.lastInputAt, now); cached.dirty = true; cached.warnedDeadline = null;
    if (this.#activity.has(row.tokenHash) || this.#activity.size < this.#policy.maximumActivityEntries) this.#activity.set(row.tokenHash, cached);
    else cached.lastPersistAt = 0; // Capacity fallback persists this event; never evict dirty state.
    this.#persist(row, cached, now);
    const current = this.#client(row.tokenHash, now); if (!current) fail('session_expired');
    return this.#descriptor(current, now);
  }
  // Raw authority.currentClient must not call Store.authenticate: the latter
  // may invoke enforceCurrent as its trusted client-only authentication guard.
  describe(hash) {
    const now = this.#time(), row = this.#client(hash, now);
    return !row || (row.idleEnabled && now >= this.#last(row) + this.#policy.idleTimeoutMs) ? null : this.#descriptor(row, now);
  }
  enforceCurrent(hash) {
    try {
      const now = this.#time(); let row = this.#client(hash, now); if (!row) return false;
      const cached = this.#activity.get(hash); if (cached) { this.#persist(row, cached, now); row = this.#client(hash, now); if (!row) return false; }
      if (!row.idleEnabled || now < this.#last(row) + this.#policy.idleTimeoutMs) return true;
      if (this.#endIdle(row, now)) return false;
      row = this.#client(hash, now);
      return !!row && (!row.idleEnabled || now < this.#last(row) + this.#policy.idleTimeoutMs);
    } catch { return false; }
  }
  sweepIdle() {
    const now = this.#time(), candidates = sync(this.#authority.idleCandidates(now, this.#policy.sweepBatch));
    if (!Array.isArray(candidates) || candidates.length > this.#policy.sweepBatch || new Set(candidates).size !== candidates.length || candidates.some(hash => typeof hash !== 'string' || !hashPattern.test(hash))) fail('session_activity_unavailable');
    const result = { ended: [], warnings: [], unavailable: [] };
    for (const hash of candidates) {
      try {
        let row = this.#client(hash, now); if (!row) continue;
        const cached = this.#activity.get(hash); if (cached) { this.#persist(row, cached, now); row = this.#client(hash, now); if (!row) continue; }
        if (!row.idleEnabled) continue;
        const deadline = this.#last(row) + this.#policy.idleTimeoutMs;
        if (now >= deadline) {
          if (this.#endIdle(row, now)) result.ended.push({ tokenHash: hash, userId: row.userId, reason: sessionIdleTimeout });
          continue;
        }
        if (now >= deadline - this.#policy.warningMs && cached?.warnedDeadline !== deadline) {
          if (cached) cached.warnedDeadline = deadline;
          else if (this.#activity.size < this.#policy.maximumActivityEntries) this.#activity.set(hash, { userId: row.userId, createdAt: row.createdAt,
            lastInputAt: this.#last(row), lastPersistAt: 0, dirty: false, warnedDeadline: deadline });
          result.warnings.push({ tokenHash: hash, userId: row.userId, ...this.#descriptor(row, now) });
        }
      } catch (error) { result.unavailable.push({ tokenHash: hash, code: error instanceof SessionAccessError ? error.code : 'session_activity_unavailable' }); }
    }
    return result;
  }
  // Only a server-minted authenticated UI proof can change THIS session's
  // policy. A successful explicit toggle is itself real accepted UI activity;
  // unlike high-frequency remote input it is durably written in the same CAS.
  setIdleEnabled(proof, enabled) {
    if (typeof enabled !== 'boolean') fail('invalid_session_access_input');
    const now = this.#time(), accepted = sync(this.#authority.acceptedInput(proof, now));
    if (!accepted || typeof accepted.tokenHash !== 'string' || !hashPattern.test(accepted.tokenHash) ||
        typeof accepted.userId !== 'string' || !identityPattern.test(accepted.userId) || accepted.kind !== 'ui') fail('session_activity_not_admitted');
    let row = this.#client(accepted.tokenHash, now);
    if (!row || row.userId !== accepted.userId) fail('session_expired');
    if (row.idleEnabled && now >= this.#last(row) + this.#policy.idleTimeoutMs) {
      if (this.#endIdle(row, now)) fail(sessionIdleTimeout);
      row = this.#client(accepted.tokenHash, now);
      if (!row || (row.idleEnabled && now >= this.#last(row) + this.#policy.idleTimeoutMs)) fail('session_activity_unavailable');
    }
    let checked = false, approved = false;
    const validateCurrent = actual => {
      checked = true;
      approved = !!actual && actual.tokenHash === row.tokenHash && actual.userId === row.userId && actual.purpose === 'client' &&
        actual.createdAt === row.createdAt && actual.idleEnabled === row.idleEnabled && actual.lastInputAt === row.lastInputAt && actual.activityStartedAt === row.activityStartedAt &&
        integer(actual.expiresAt, row.createdAt + 1) && actual.expiresAt > now && integer(actual.activityStartedAt, row.createdAt, now) &&
        (actual.lastInputAt === null || integer(actual.lastInputAt, actual.activityStartedAt, now)) &&
        (!actual.idleEnabled || now < this.#last(actual) + this.#policy.idleTimeoutMs);
      return approved;
    };
    const saved = sync(this.#authority.setIdleEnabledCAS({ tokenHash: row.tokenHash, userId: row.userId, createdAt: row.createdAt,
      expectedIdleEnabled: row.idleEnabled, expectedLastInputAt: row.lastInputAt, enabled, lastInputAt: now, now, validateCurrent }));
    if (!checked || !approved || saved !== true) fail('session_access_commit_unverified');
    // No stale warned deadline or dirty pre-toggle timestamp survives a durable
    // toggle. Real input while disabled is otherwise still tracked/coalesced.
    this.#activity.delete(row.tokenHash);
    const current = this.#client(row.tokenHash, now);
    if (!current || current.userId !== row.userId || current.createdAt !== row.createdAt || current.idleEnabled !== enabled || current.lastInputAt !== now) fail('session_access_commit_unverified');
    return this.#descriptor(current, now);
  }
}

export function terminalSessionReason(value) { return value === pcLoginReplaced || value === sessionIdleTimeout ? value : null; }
