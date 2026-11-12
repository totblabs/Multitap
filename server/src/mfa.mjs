// PRIVATE integration candidate. No factor is enabled by constructing this class.
import { createHash, randomBytes } from 'node:crypto';
import { performance } from 'node:perf_hooks';
import { HttpError, fields } from './errors.mjs';
import { verifyPassword, tokenHash } from './security.mjs';
import { matchingStep, sealFactor as sealValue, openFactor as openValue, createRecoveryCodes as recoveryValues, recoveryDigest } from './mfa-primitives.mjs';

const ttl = 120000, tokenPattern = /^[A-Za-z0-9_-]{43}$/;
// Failed attempts: five within a minute block the account for 60 s, and each
// further block in a row doubles (2, 4, 8 ... minutes, at most 1 h) until a
// code is accepted. After 30 failed authenticator codes within 24 h only a
// recovery code is accepted until those 24 h have passed.
const day = 86400000, dayCap = 30, blockMs = streak => Math.min(60000 * 2 ** Math.max(0, streak - 1), 3600000);
const digest = value => createHash('sha256').update(value).digest('hex');
const fail = (status, code) => { throw new HttpError(status, code); };
const available = action => { try { return action(); } catch { fail(503, 'mfa_unavailable'); } };
const sealFactor = (...args) => available(() => sealValue(...args));
const openFactor = (...args) => available(() => openValue(...args));
const createRecoveryCodes = (...args) => available(() => recoveryValues(...args));
const sync = value => { if (value && typeof value.then === 'function') throw Error('mfa_async_authority'); return value; };
function base32(bytes) {
  const chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'; let bits = 0, value = 0, result = '';
  for (const byte of bytes) { value = (value << 8) | byte; bits += 8; while (bits >= 5) { result += chars[(value >>> (bits -= 5)) & 31]; } }
  if (bits) result += chars[(value << (5 - bits)) & 31]; return result;
}
export class MfaProvider {
  #store; #vault; #now; #mono; #random; #changed;
  #challenges = new Map(); #setups = new Map(); #recent = new Map();
  constructor({ store, vault, now = Date.now, monotonic = () => performance.now(), random = randomBytes, changed = () => {} }) {
    this.#store = store; this.#vault = vault; this.#now = now; this.#mono = monotonic; this.#random = random; this.#changed = changed;
  }
  guard(context) {
    // Context is built only by http.mjs after actual request-origin resolution.
    if (context.secure !== true || !context.origin?.startsWith('https://')) fail(403, 'mfa_requires_https');
  }
  #time() {
    const wall = this.#now(), mono = this.#mono();
    if (!Number.isSafeInteger(wall) || wall < 0 || !Number.isFinite(mono) || mono < 0) fail(503, 'mfa_unavailable');
    return { wall, mono };
  }
  #account(id, allowDisabled = false) {
    const user = this.#store.db.prepare('SELECT * FROM users WHERE id=?').get(id);
    if (!user || (user.disabled && !allowDisabled)) fail(401, 'session_expired');
    const factor = this.#store.db.prepare('SELECT * FROM account_mfa WHERE user_id=?').get(id);
    return { user, factor, generation: factor?.generation ?? 0 };
  }
  #snapshot(user, generation) { return { userId: user.id, password: digest(user.password_hash), revision: user.authorization_revision, generation }; }
  #same(bound, actual) {
    return bound.userId === actual.user.id && bound.password === digest(actual.user.password_hash) &&
      bound.revision === actual.user.authorization_revision && bound.generation === actual.generation;
  }
  #valid(bound, context) {
    const time = this.#time();
    if (!bound || time.wall < bound.at || time.wall >= bound.until || time.mono < bound.mono || time.mono >= bound.mono + ttl) fail(410, 'mfa_challenge_expired');
    if (bound.origin !== context.origin || bound.purpose !== context.purpose) fail(403, 'origin_not_allowed');
    const actual = this.#account(bound.userId);
    if (!this.#same(bound, actual) || (bound.purpose === 'admin' && actual.user.role !== 'admin')) fail(410, 'mfa_challenge_expired');
    return actual;
  }
  #prune(map) {
    const time = this.#time();
    for (const [key, item] of map) if (time.wall < item.at || time.wall >= item.until || time.mono < item.mono || time.mono >= item.mono + ttl) map.delete(key);
  }
  #allocate(map, value) {
    this.#prune(map);
    if (map.size >= 1024 || [...map.values()].filter(row => row.userId === value.userId).length >= 4) fail(429, 'too_many_attempts');
    for (let i = 0; i < 4; i++) {
      const token = this.#random(32).toString('base64url');
      if (!tokenPattern.test(token) || map.has(tokenHash(token))) continue;
      map.set(tokenHash(token), value); return token;
    }
    fail(503, 'mfa_unavailable');
  }
  enabled(userId) { return this.#store.db.prepare('SELECT enabled FROM account_mfa WHERE user_id=?').get(userId)?.enabled === 1; }
  issue(user, context, policy, loginContextRequested) {
    this.guard(context);
    const actual = this.#account(user.id);
    if (actual.user.password_hash !== user.password_hash || actual.user.authorization_revision !== user.authorization_revision || !actual.factor?.enabled) fail(401, 'invalid_credentials');
    const time = this.#time(), challengeToken = this.#allocate(this.#challenges, {
      ...this.#snapshot(actual.user, actual.generation), ...time, at: time.wall, until: time.wall + ttl,
      origin: context.origin, purpose: context.purpose, policy: structuredClone(policy), loginContextRequested, attempts: 0,
    });
    return { mfa: { protocol: 'mfa-v1', challengeToken, expiresAt: new Date(time.wall + ttl).toISOString(), methods: ['totp', 'recovery'] } };
  }
  #budget(userId) {
    const now = this.#time().wall, old = this.#store.db.prepare('SELECT * FROM mfa_failure_budgets WHERE user_id=?').get(userId);
    const effective = Math.max(now, old?.last_observed_at ?? now);
    // The day of the daily cap starts with its first failed code; the blocks
    // in a row are counted inside that day too.
    const inDay = !!old && old.day_started_at > 0 && effective < old.day_started_at + day;
    return { userId, at: effective, start: old && effective < old.window_started_at + 60000 ? old.window_started_at : effective,
      failures: old && effective < old.window_started_at + 60000 ? old.failures : 0, blocked: old?.blocked_until ?? 0,
      streak: inDay || (old && effective < old.blocked_until + day) ? old?.block_streak ?? 0 : 0,
      dayStart: inDay ? old.day_started_at : 0, dayFailures: inDay ? old.day_failures : 0 };
  }
  #saveBudget(value) {
    this.#store.db.prepare(`INSERT INTO mfa_failure_budgets(user_id,window_started_at,failures,blocked_until,last_observed_at,block_streak,day_started_at,day_failures) VALUES(?,?,?,?,?,?,?,?)
      ON CONFLICT(user_id) DO UPDATE SET window_started_at=excluded.window_started_at,failures=excluded.failures,blocked_until=excluded.blocked_until,last_observed_at=excluded.last_observed_at,
        block_streak=excluded.block_streak,day_started_at=excluded.day_started_at,day_failures=excluded.day_failures`)
      .run(value.userId, value.start, value.failures, value.blocked, value.at, value.streak, value.dayStart, value.dayFailures);
  }
  // One failed attempt. kind: 'code' (authenticator code at sign-in or
  // re-check), 'recovery', 'password' or 'setup'. Returns true when it
  // blocked the account. Only authenticator codes count towards the daily cap.
  #fail(budget, kind) {
    budget.failures = Math.min(5, budget.failures + 1);
    if (kind === 'code') {
      if (!budget.dayStart) budget.dayStart = budget.at;
      budget.dayFailures = Math.min(dayCap, budget.dayFailures + 1);
      if (budget.dayFailures === dayCap) this.#store.audit('mfa_locked', budget.userId, budget.userId + ':day', budget.at);
    }
    if (kind === 'code' || kind === 'recovery') this.#auditFailure(budget.userId, budget.at);
    if (budget.failures !== 5) return false;
    budget.streak = Math.min(budget.streak + 1, 7);
    const ms = blockMs(budget.streak);
    budget.blocked = Math.max(budget.blocked, budget.at + ms);
    this.#store.audit('mfa_locked', budget.userId, budget.userId + ':' + Math.round(ms / 60000), budget.at);
    return true;
  }
  // mfa_failed is grouped: at most one row per account per minute, the
  // number of failures in its subject (userId:count).
  #auditFailure(userId, at) {
    const row = this.#store.db.prepare("SELECT id,subject FROM audit_events WHERE event='mfa_failed' AND actor=? AND created_at>? AND created_at<=? ORDER BY id DESC LIMIT 1").get(userId, at - 60000, at);
    const count = row ? Number(String(row.subject).split(':')[1]) : 0;
    if (row && Number.isSafeInteger(count) && count > 0) this.#store.db.prepare('UPDATE audit_events SET subject=? WHERE id=?').run(userId + ':' + (count + 1), row.id);
    else this.#store.audit('mfa_failed', userId, userId + ':1', at);
  }
  #consume(actual, input) {
    const budget = this.#budget(actual.user.id);
    if (budget.blocked > budget.at) { this.#saveBudget(budget); return { error: new HttpError(429, 'too_many_attempts') }; }
    // The daily cap: authenticator codes are not even checked; a recovery code still works.
    if (input.code !== undefined && budget.dayFailures >= dayCap) { this.#saveBudget(budget); return { error: new HttpError(429, 'too_many_attempts', 'too_many_attempts', { recoveryCodeOnly: true }) }; }
    let accepted = false;
    if (input.code !== undefined) {
      const secret = openFactor(this.#vault, actual.user.id, actual.generation, actual.factor.factor_ciphertext);
      try {
        const step = matchingStep(secret, input.code, budget.at, actual.factor.last_accepted_step);
        if (step !== null) accepted = this.#store.db.prepare('UPDATE account_mfa SET last_accepted_step=? WHERE user_id=? AND generation=? AND last_accepted_step=?')
          .run(step, actual.user.id, actual.generation, actual.factor.last_accepted_step).changes === 1;
      } finally { secret.fill(0); }
    } else {
      const hash = recoveryDigest(actual.user.id, actual.generation, input.recoveryCode);
      accepted = this.#store.db.prepare('UPDATE mfa_recovery_codes SET consumed_at=? WHERE user_id=? AND generation=? AND code_hash=? AND consumed_at IS NULL')
        .run(budget.at, actual.user.id, actual.generation, hash).changes === 1;
    }
    if (!accepted) {
      const blocked = this.#fail(budget, input.code !== undefined ? 'code' : 'recovery');
      this.#saveBudget(budget);
      const recoveryOnly = input.code !== undefined && budget.dayFailures >= dayCap;
      return { error: blocked || recoveryOnly ? new HttpError(429, 'too_many_attempts', 'too_many_attempts', recoveryOnly ? { recoveryCodeOnly: true } : undefined) : new HttpError(400, 'mfa_invalid_code') };
    }
    // Accepted: the blocks in a row start again. The daily count stays until its day has passed.
    budget.failures = 0; budget.blocked = 0; budget.streak = 0; this.#saveBudget(budget); return { accepted: true };
  }
  #codeFields(input) {
    if ((input.code === undefined) === (input.recoveryCode === undefined)) fail(400, 'invalid_mfa_fields');
    if (input.code !== undefined && (typeof input.code !== 'string' || !/^[0-9]{6}$/.test(input.code))) fail(400, 'invalid_mfa_fields');
    if (input.recoveryCode !== undefined && (typeof input.recoveryCode !== 'string' || !/^[a-fA-F0-9]{32}$/.test(input.recoveryCode))) fail(400, 'invalid_mfa_fields');
  }
  verify(input, context, finish, rollback = () => {}) {
    this.guard(context); fields(input, ['challengeToken', 'code', 'recoveryCode']); this.#codeFields(input);
    if (!tokenPattern.test(input.challengeToken)) fail(410, 'mfa_challenge_expired');
    const key = tokenHash(input.challengeToken), bound = this.#challenges.get(key);
    this.#valid(bound, context);
    let pendingConfirmation;
    let outcome;
    try {
      outcome = this.#store.transaction(() => {
        const actual = this.#valid(bound, context);
        if (!actual.factor?.enabled) fail(410, 'mfa_challenge_expired');
        const checked = this.#consume(actual, input);
        if (checked.error) return checked; // Commit failed-attempt budget before HTTP throw.
        try { return { session: sync(finish(actual.user, bound.policy, bound.loginContextRequested)) }; }
        catch (error) {
          if (error instanceof HttpError && error.code === 'login_confirmation_required') {
            pendingConfirmation = error.details?.challengeToken;
            return { confirmation: error }; // A code succeeds without evicting old sessions.
          }
          throw error;
        }
      });
    } catch (error) { if (pendingConfirmation) rollback(pendingConfirmation); throw error; }
    if (outcome.error) { if (++bound.attempts >= 5) this.#challenges.delete(key); throw outcome.error; }
    this.#challenges.delete(key);
    if (outcome.confirmation) throw outcome.confirmation;
    return outcome.session;
  }
  #principal(principal) {
    const current = this.#store.authenticate(principal.hash, this.#time().wall, principal.admin ? 'admin' : 'client');
    if (!current || current.id !== principal.user.id || current.must_change_credentials) fail(401, 'session_expired');
    return this.#account(current.id);
  }
  async #password(principal, input, passwordJob) {
    const before = this.#principal(principal);
    if (typeof input.currentPassword !== 'string' || !input.currentPassword || input.currentPassword.length > 256) fail(400, 'invalid_mfa_fields');
    const permitted = this.#store.transaction(() => {
      const budget = this.#budget(before.user.id); this.#saveBudget(budget);
      return budget.blocked <= budget.at;
    });
    if (!permitted) fail(429, 'too_many_attempts');
    let okay;
    try { okay = await passwordJob(() => verifyPassword(input.currentPassword, before.user.password_hash)); }
    finally { input.currentPassword = ''; }
    const current = this.#principal(principal);
    if (!okay) {
      const locked = this.#store.transaction(() => {
        this.#principal(principal);
        const budget = this.#budget(current.user.id);
        this.#fail(budget, 'password');
        this.#saveBudget(budget); return budget.blocked > budget.at;
      });
      fail(locked ? 429 : 403, locked ? 'too_many_attempts' : 'current_password_incorrect');
    }
    if (!this.#same(this.#snapshot(before.user, before.generation), current)) fail(401, 'session_expired');
    return current;
  }
  #recentRequired(principal, actual) {
    const proof = this.#recent.get(principal.hash), now = this.#time();
    if (!proof || proof.origin !== principal.origin || !this.#same(proof, actual) || now.mono < proof.mono || now.mono >= proof.mono + 300000 || now.wall < proof.at || now.wall >= proof.at + 300000) fail(403, 'mfa_reauth_required');
  }
  #replace(actual, generation, ciphertext, step, codes) {
    const id = actual.user.id, now = this.#time().wall;
    this.#store.db.prepare(`INSERT INTO account_mfa(user_id,generation,enabled,factor_ciphertext,last_accepted_step,changed_at) VALUES(?,?,?,?,?,?)
      ON CONFLICT(user_id) DO UPDATE SET generation=excluded.generation,enabled=excluded.enabled,factor_ciphertext=excluded.factor_ciphertext,last_accepted_step=excluded.last_accepted_step,changed_at=excluded.changed_at`)
      .run(id, generation, ciphertext === null ? 0 : 1, ciphertext, step, now);
    this.#store.db.prepare('DELETE FROM mfa_recovery_codes WHERE user_id=?').run(id);
    for (const hash of codes?.hashes ?? []) this.#store.db.prepare('INSERT INTO mfa_recovery_codes(user_id,generation,code_hash) VALUES(?,?,?)').run(id, generation, hash);
    this.#store.db.prepare('UPDATE users SET authorization_revision=authorization_revision+1 WHERE id=?').run(id);
    this.#store.db.prepare('DELETE FROM sessions WHERE user_id=?').run(id);
    this.#store.audit('mfa_factor_changed', id, id, now);
  }
  #invalidate(userId) {
    for (const map of [this.#challenges, this.#setups, this.#recent]) for (const [key, value] of map) if (value.userId === userId) map.delete(key);
    this.#changed(userId);
  }
  async manage(action, input, principal, passwordJob) {
    this.guard({ origin: principal.origin, secure: principal.secure });
    const original = this.#principal(principal);
    if (action === 'status') {
      return { protocol: 'mfa-v1', enabled: original.factor?.enabled === 1,
        recoveryCodesRemaining: this.#store.db.prepare('SELECT count(*) n FROM mfa_recovery_codes WHERE user_id=? AND generation=? AND consumed_at IS NULL').get(original.user.id, original.generation).n };
    }
    if (action === 'setup') {
      fields(input, ['currentPassword']); const actual = await this.#password(principal, input, passwordJob);
      if (actual.factor?.enabled) this.#recentRequired(principal, actual);
      const secret = this.#random(20), generation = actual.generation + 1, time = this.#time();
      try {
        const value = { ...this.#snapshot(actual.user, actual.generation), origin: principal.origin, purpose: principal.purpose,
          at: time.wall, mono: time.mono, until: time.wall + ttl, sessionHash: principal.hash, nextGeneration: generation,
          ciphertext: sealFactor(this.#vault, actual.user.id, generation, secret), attempts: 0 };
        const setupToken = this.#allocate(this.#setups, value), encoded = base32(secret);
        const label = encodeURIComponent('Multitap:' + actual.user.username);
        return { protocol: 'mfa-v1', setupToken, expiresAt: new Date(value.until).toISOString(), secret: encoded,
          uri: `otpauth://totp/${label}?secret=${encoded}&issuer=Multitap&algorithm=SHA1&digits=6&period=30` };
      } finally { secret.fill(0); }
    }
    if (action === 'enable') {
      fields(input, ['setupToken', 'code']);
      if (!tokenPattern.test(input.setupToken) || typeof input.code !== 'string' || !/^[0-9]{6}$/.test(input.code)) fail(400, 'invalid_mfa_fields');
      const key = tokenHash(input.setupToken), setup = this.#setups.get(key);
      const actual = this.#valid(setup, { origin: principal.origin, purpose: principal.purpose });
      if (setup.sessionHash !== principal.hash || actual.user.id !== principal.user.id) fail(410, 'mfa_challenge_expired');
      if (actual.factor?.enabled) this.#recentRequired(principal, actual);
      const outcome = this.#store.transaction(() => {
        this.#principal(principal); const current = this.#valid(setup, { origin: principal.origin, purpose: principal.purpose });
        // BEGIN can wait beyond the preflight proof deadline. Re-check the
        // exact current session/factor proof after that wait, before replacing.
        if (current.factor?.enabled) this.#recentRequired(principal, current);
        const budget = this.#budget(current.user.id);
        if (budget.blocked > budget.at) { this.#saveBudget(budget); return { error: new HttpError(429, 'too_many_attempts') }; }
        const secret = openFactor(this.#vault, current.user.id, setup.nextGeneration, setup.ciphertext);
        let step; try { step = matchingStep(secret, input.code, budget.at); } finally { secret.fill(0); }
        if (step === null) {
          const blocked = this.#fail(budget, 'setup');
          this.#saveBudget(budget); return { error: new HttpError(blocked ? 429 : 400, blocked ? 'too_many_attempts' : 'mfa_invalid_code') };
        }
        const codes = createRecoveryCodes(current.user.id, setup.nextGeneration, this.#random);
        this.#replace(current, setup.nextGeneration, setup.ciphertext, step, codes); return { codes: codes.codes };
      });
      if (outcome.error) { if (++setup.attempts >= 5) this.#setups.delete(key); throw outcome.error; }
      this.#setups.delete(key); this.#invalidate(actual.user.id);
      return { enabled: true, recoveryCodes: outcome.codes, requiresLogin: true };
    }
    if (!['reauth', 'disable', 'recovery'].includes(action)) fail(404, 'not_found');
    fields(input, action === 'reauth' ? ['currentPassword', 'code', 'recoveryCode'] : ['currentPassword']);
    if (action === 'reauth') this.#codeFields(input);
    const actual = await this.#password(principal, input, passwordJob);
    if (!actual.factor?.enabled) fail(409, 'mfa_not_enabled');
    if (action === 'reauth') {
      const time = this.#time();
      for (const [key, proof] of this.#recent) if (time.wall < proof.at || time.wall >= proof.at + 300000 || time.mono < proof.mono || time.mono >= proof.mono + 300000) this.#recent.delete(key);
      if (!this.#recent.has(principal.hash) && this.#recent.size >= 4096) fail(503, 'mfa_unavailable');
      const result = this.#store.transaction(() => { const fresh = this.#principal(principal); return this.#consume(fresh, input); });
      if (result.error) throw result.error;
      this.#recent.set(principal.hash, { ...this.#snapshot(actual.user, actual.generation), origin: principal.origin, ...time, at: time.wall });
      return { ok: true };
    }
    this.#recentRequired(principal, actual);
    const result = this.#store.transaction(() => {
      const current = this.#principal(principal); this.#recentRequired(principal, current);
      const next = current.generation + 1;
      if (action === 'disable') { this.#replace(current, next, null, -1, null); return { enabled: false, requiresLogin: true }; }
      const secret = openFactor(this.#vault, current.user.id, current.generation, current.factor.factor_ciphertext);
      try {
        const codes = createRecoveryCodes(current.user.id, next, this.#random);
        this.#replace(current, next, sealFactor(this.#vault, current.user.id, next, secret), current.factor.last_accepted_step, codes);
        return { enabled: true, recoveryCodes: codes.codes, requiresLogin: true };
      } finally { secret.fill(0); }
    });
    this.#invalidate(actual.user.id); return result;
  }
  resetLocal(userId, confirmUserId) {
    if (userId !== confirmUserId || !/^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/.test(userId)) throw Error('mfa_reset_confirmation_required');
    this.#store.transaction(() => {
      const actual = this.#account(userId, true); this.#replace(actual, actual.generation + 1, null, -1, null);
      this.#store.audit('mfa_root_reset', 'server-admin', userId, this.#time().wall);
    });
    this.#invalidate(userId); return { reset: true, requiresLogin: true };
  }
}
