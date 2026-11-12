import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { SessionAccess, SessionAccessError, sessionAccessProtocol, sessionAccessDefaults, sessionIdleTimeout, pcLoginReplaced, terminalSessionReason } from '../src/session-access.mjs';

// These are real module algorithms over an explicitly transactional MEMORY
// adapter. They do not claim Store/schema18/HTTP or installed-client acceptance.
const tokenHash = n => createHash('sha256').update('synthetic-session-' + n).digest('hex');
const clone = value => structuredClone(value);
const fails = code => error => error instanceof SessionAccessError && error.code === code;
function fixture(policy = {}) {
  let now = 1700000000000, serial = 0, random = 0;
  const user = { id: 'fixture-user', password_hash: 'synthetic-stored-password-hash', disabled: 0, must_change_credentials: 0 };
  const rows = new Map(), reasons = new Map(), inputProofs = new WeakMap();
  const counts = { commits: 0, persist: 0, ends: 0, toggles: 0 };
  const hooks = { beforeCommit: null, failCommit: false, omitCommitGuard: false, beforeEnd: null, beforeToggle: null };
  const live = row => row && row.expiresAt > now;
  const activeClients = id => [...rows.values()].filter(row => live(row) && row.userId === id && row.purpose === 'client');
  const authority = {
    account: id => id === user.id ? clone(user) : null,
    clientSessions: id => clone(activeClients(id)),
    commitLogin(value) {
      ++counts.commits; hooks.beforeCommit?.();
      if (hooks.omitCommitGuard) return true;
      const currentSessions = activeClients(value.accountId);
      value.validateCurrent({ account: clone(user), currentSessions: clone(currentSessions), totalSessionCount: [...rows.values()].filter(row => live(row) && row.userId === value.accountId).length });
      const draft = new Map([...rows].map(([key, row]) => [key, clone(row)])), ends = new Map(reasons);
      for (const row of value.expectedSessions) { draft.delete(row.tokenHash); ends.set(row.tokenHash, { tokenHash: row.tokenHash, userId: row.userId, reason: value.reason }); }
      draft.set(value.newSession.tokenHash, { ...value.newSession, userId: value.accountId, activityStartedAt: value.newSession.createdAt, lastInputAt: null, idleEnabled: true });
      if (hooks.failCommit) throw Error('synthetic_transaction_rollback');
      rows.clear(); for (const [key, row] of draft) rows.set(key, row);
      reasons.clear(); for (const [key, reason] of ends) reasons.set(key, reason);
      return true;
    },
    currentClient(hash) { const row = rows.get(hash); return live(row) && row.purpose === 'client' ? clone(row) : null; },
    acceptedInput(proof) { const accepted = inputProofs.get(proof); inputProofs.delete(proof); return accepted ?? null; },
    persistInputCAS(value) {
      const row = rows.get(value.tokenHash);
      if (!live(row) || row.purpose !== 'client' || row.userId !== value.userId || row.createdAt !== value.createdAt || row.lastInputAt !== value.expectedLastInputAt) return false;
      ++counts.persist; row.lastInputAt = Math.max(row.lastInputAt ?? row.activityStartedAt, value.lastInputAt); return true;
    },
    endIdleCAS(value) {
      ++counts.ends; hooks.beforeEnd?.();
      const row = rows.get(value.tokenHash);
      const approved = value.validateCurrent(row ? clone(row) : null);
      if (!approved) return false;
      rows.delete(value.tokenHash); reasons.set(value.tokenHash, { tokenHash: value.tokenHash, userId: value.userId, reason: value.reason }); return true;
    },
    setIdleEnabledCAS(value) {
      hooks.beforeToggle?.();
      const row = rows.get(value.tokenHash);
      if (!value.validateCurrent(row ? clone(row) : null)) return false;
      assert.equal(row.idleEnabled, value.expectedIdleEnabled); assert.equal(row.lastInputAt, value.expectedLastInputAt);
      ++counts.toggles; row.idleEnabled = value.enabled; row.lastInputAt = value.lastInputAt; return true;
    },
    idleCandidates: (_now, limit) => [...rows.values()].filter(row => live(row) && row.purpose === 'client').slice(0, limit).map(row => row.tokenHash),
  };
  const make = () => new SessionAccess({ authority, policy, now: () => now, randomBytes: n => { const bytes = Buffer.alloc(n); bytes.writeUInt32BE(++random); return bytes; } });
  const api = make();
  function add(issuer = 'legacy', purpose = 'client', extra = {}) {
    const hash = tokenHash(++serial);
    rows.set(hash, { tokenHash: hash, userId: user.id, purpose, issuer, createdAt: now, expiresAt: now + 86400000, activityStartedAt: now, lastInputAt: null, idleEnabled: true, ...extra });
    return hash;
  }
  const next = (issuer = 'windows-pc-v1') => ({ tokenHash: tokenHash(++serial), ttlMs: 28800000, issuer });
  const input = (hash, kind = 'remote-input', overrides = {}) => { const proof = Object.freeze({}); inputProofs.set(proof, { tokenHash: hash, userId: user.id, kind, ...overrides }); return proof; };
  return { api, make, user, rows, reasons, counts, hooks, authority, add, next, input, now: () => now, advance: ms => { now += ms; } };
}

test('direct admission is non-forgeable/single-use and verifies fresh account/set in the mandatory transaction guard', () => {
  const f = fixture(), value = f.next(), begin = f.api.begin(clone(f.user));
  assert.equal(begin.kind, 'ready'); assert.equal(begin.protocol, sessionAccessProtocol);
  assert.throws(() => f.api.commitDirect({}, value), fails('login_confirmation_invalid'));
  assert.equal(f.api.commitDirect(begin.admission, value).created, true);
  assert.equal(f.rows.get(value.tokenHash).purpose, 'client'); assert.equal(f.counts.commits, 1);
  assert.throws(() => f.api.commitDirect(begin.admission, f.next()), fails('login_confirmation_invalid'));
});

test('confirmation replaces all client issuers, including browser console, while retaining admin authentication', () => {
  const f = fixture(), old = [f.add('windows-pc-v1'), f.add('legacy'), f.add('browser-console')], admin = f.add('admin', 'admin');
  const offer = f.api.begin(clone(f.user));
  assert.equal(offer.kind, 'confirmation-required'); assert.equal(offer.sessions, 3); assert.match(offer.challenge, /^[A-Za-z0-9_-]{43}$/);
  assert.equal(f.rows.size, 4); assert.equal(f.counts.commits, 0);
  const value = f.next('legacy'), result = f.api.confirm(offer.challenge, value);
  assert.equal(result.replaced, 3); assert.equal(result.reason, pcLoginReplaced); assert.ok(f.rows.has(admin)); assert.ok(f.rows.has(value.tokenHash));
  for (const hash of old) { assert.equal(f.rows.has(hash), false); assert.equal(f.reasons.get(hash).reason, 'pc_login_replaced'); }
  assert.throws(() => f.api.confirm(offer.challenge, f.next()), fails('login_confirmation_invalid'));
});

for (const mode of ['cancel', 'expired', 'password', 'disabled', 'account-id', 'new-client', 'removed-client']) test('confirmation '+mode+' never evicts an observed owner', () => {
  const f = fixture(), old = f.add(), offer = f.api.begin(clone(f.user));
  if (mode === 'cancel') f.api.cancel(offer.challenge);
  else if (mode === 'expired') f.advance(120000);
  else if (mode === 'password') f.user.password_hash = 'different-synthetic-stored-hash';
  else if (mode === 'disabled') f.user.disabled = 1;
  else if (mode === 'account-id') f.user.id = 'different-immutable-account';
  else if (mode === 'new-client') f.add('browser-console');
  else f.rows.delete(old);
  assert.throws(() => f.api.confirm(offer.challenge, f.next()), error => error instanceof SessionAccessError);
  if (mode !== 'removed-client') assert.ok(f.rows.has(old)); assert.equal(f.counts.commits, 0); assert.equal(f.reasons.size, 0);
});

test('two offers bind the old set; a late confirm cannot evict the winner/new successor', () => {
  const f = fixture(), old = f.add(), a = f.api.begin(clone(f.user)), b = f.api.begin(clone(f.user)), winner = f.next();
  f.api.confirm(a.challenge, winner);
  assert.throws(() => f.api.confirm(b.challenge, f.next()), fails('login_confirmation_stale'));
  assert.ok(f.rows.has(winner.tokenHash)); assert.equal(f.rows.has(old), false); assert.equal(f.counts.commits, 1);
});

test('same identity renewal is not a new login set, and fresh transaction changes still defeat a valid precheck', () => {
  const f = fixture(), old = f.add(), offer = f.api.begin(clone(f.user)); f.rows.get(old).expiresAt += 1000;
  f.api.confirm(offer.challenge, f.next()); assert.equal(f.counts.commits, 1);
  const other = fixture(); other.add(); const pending = other.api.begin(clone(other.user));
  other.hooks.beforeCommit = () => other.add('browser-console');
  assert.throws(() => other.api.confirm(pending.challenge, other.next()), fails('login_confirmation_stale'));
  assert.equal(other.reasons.size, 0); assert.equal(other.rows.size, 2);
});

test('atomic adapter failure leaves old auth/reasons untouched, and the challenge is not replayable', () => {
  const f = fixture(), old = f.add(), offer = f.api.begin(clone(f.user)); f.hooks.failCommit = true;
  assert.throws(() => f.api.confirm(offer.challenge, f.next()), /synthetic_transaction_rollback/);
  assert.ok(f.rows.has(old)); assert.equal(f.rows.size, 1); assert.equal(f.reasons.size, 0);
  assert.throws(() => f.api.confirm(offer.challenge, f.next()), fails('login_confirmation_invalid'));
});

test('missing transaction validation and capacity made only of admin sessions never produce a client admission', () => {
  const f = fixture(); f.add(); const offer = f.api.begin(clone(f.user)); f.hooks.omitCommitGuard = true;
  assert.throws(() => f.api.confirm(offer.challenge, f.next()), fails('session_access_commit_unverified'));
  const capped = fixture(); for (let i = 0; i < 5; ++i) capped.add('admin', 'admin');
  const ready = capped.api.begin(clone(capped.user));
  assert.throws(() => capped.api.commitDirect(ready.admission, capped.next()), fails('active_session_limit'));
  assert.equal(capped.rows.size, 5); assert.equal(capped.reasons.size, 0);
});

test('confirmation map is bounded and expiration releases capacity without evicting authentication', () => {
  const f = fixture({ maximumChallenges: 2, challengesPerAccount: 2 }); f.add(); f.api.begin(clone(f.user)); f.api.begin(clone(f.user));
  assert.throws(() => f.api.begin(clone(f.user)), fails('login_confirmation_limit'));
  f.advance(120000); assert.equal(f.api.begin(clone(f.user)).kind, 'confirmation-required'); assert.equal(f.rows.size, 1); assert.equal(f.reasons.size, 0);
});

test('accepted real input uses server UTC, rejects forged/foreign/replayed proofs and never records payload data', () => {
  const f = fixture(), hash = f.add();
  assert.throws(() => f.api.recordAcceptedInput({ tokenHash: hash, userId: f.user.id, kind: 'ui', active: true, timestamp: f.now() + 1e9 }), fails('session_activity_not_admitted'));
  assert.throws(() => f.api.recordAcceptedInput(f.input(hash, 'frames')), fails('session_activity_not_admitted'));
  assert.throws(() => f.api.recordAcceptedInput(f.input(hash, 'ui', { userId: 'different-user' })), fails('session_expired'));
  const proof = f.input(hash); f.advance(1000); const descriptor = f.api.recordAcceptedInput(proof);
  assert.equal(f.rows.get(hash).lastInputAt, f.now()); assert.equal(descriptor.idleDeadline, new Date(f.now() + 1800000).toISOString());
  assert.throws(() => f.api.recordAcceptedInput(proof), fails('session_activity_not_admitted'));
  assert.equal(f.counts.persist, 1);
});

test('warning and expiry include an attached remote; renewal/frame/keepalive fields do not postpone idle', () => {
  const f = fixture(), hash = f.add('windows-pc-v1', 'client', { attachedRemote: true, frames: 100000, keepaliveAt: f.now() });
  f.advance(1800000 - 60000); f.rows.get(hash).expiresAt += 3600000; f.rows.get(hash).keepaliveAt = f.now();
  const warning = f.api.sweepIdle(); assert.equal(warning.warnings.length, 1); assert.equal(warning.warnings[0].tokenHash, hash);
  assert.equal(f.api.sweepIdle().warnings.length, 0); assert.equal(f.rows.get(hash).lastInputAt, null); assert.equal(f.counts.persist, 0);
  f.advance(60000); const ended = f.api.sweepIdle(); assert.deepEqual(ended.ended, [{ tokenHash: hash, userId: f.user.id, reason: sessionIdleTimeout }]);
  assert.equal(f.rows.has(hash), false); assert.equal(f.reasons.get(hash).reason, 'session_idle_timeout');
});

test('expiry is checked before accepting resumed input, and ordinary auth expiry cannot be revived either', () => {
  const f = fixture(), hash = f.add(); f.advance(1800000);
  assert.throws(() => f.api.recordAcceptedInput(f.input(hash)), fails('session_idle_timeout')); assert.equal(f.counts.persist, 0); assert.equal(f.rows.has(hash), false);
  const expired = fixture(), old = expired.add('legacy', 'client', { expiresAt: expired.now() + 100 }); expired.advance(100);
  assert.throws(() => expired.api.recordAcceptedInput(expired.input(old)), fails('session_expired')); assert.equal(expired.counts.persist, 0);
});

test('fresh activity at the final idle transaction guard prevents eviction after an earlier due observation', () => {
  const f = fixture(), hash = f.add(); f.advance(1800000);
  f.hooks.beforeEnd = () => { f.rows.get(hash).lastInputAt = f.now(); };
  assert.equal(f.api.enforceCurrent(hash), true); assert.ok(f.rows.has(hash)); assert.equal(f.reasons.size, 0);
});

test('RAM coalesced real input is flushed before enforcement; delayed flush cannot suppress a first newly active timestamp', () => {
  const f = fixture(), hash = f.add(); f.api.recordAcceptedInput(f.input(hash)); f.advance(1000); f.api.recordAcceptedInput(f.input(hash));
  assert.equal(f.counts.persist, 1); assert.equal(f.api.describe(hash).idleDeadline, new Date(f.now() + 1800000).toISOString());
  f.advance(100000); assert.equal(f.api.enforceCurrent(hash), true); assert.equal(f.counts.persist, 2);
  f.api.recordAcceptedInput(f.input(hash)); assert.equal(f.rows.get(hash).lastInputAt, f.now()); assert.equal(f.counts.persist, 3);
});

test('a warning-only cache does not turn tracking start into real input or defer the first late input write', () => {
  const f = fixture(), hash = f.add(); f.advance(1740000); f.api.sweepIdle(); assert.equal(f.rows.get(hash).lastInputAt, null);
  f.api.recordAcceptedInput(f.input(hash)); assert.equal(f.rows.get(hash).lastInputAt, f.now()); assert.equal(f.counts.persist, 1);
});

test('grace zero keeps the documented security-conservative crash loss bounded by the coalescing interval', () => {
  const f = fixture(), hash = f.add(); f.api.recordAcceptedInput(f.input(hash)); f.advance(1000); f.api.recordAcceptedInput(f.input(hash));
  const actualLastInput = f.now(), durableLastInput = f.rows.get(hash).lastInputAt;
  assert.ok(actualLastInput - durableLastInput > 0 && actualLastInput - durableLastInput < sessionAccessDefaults.persistInputIntervalMs);
  const restarted = f.make(); f.advance(1800000 - 1000); assert.equal(restarted.enforceCurrent(hash), false);
  assert.equal(f.now(), durableLastInput + 1800000); assert.equal(f.now() < actualLastInput + 1800000, true);
});

test('unknown/end reason remains honest, and admin rows are outside idle candidate scope', () => {
  assert.equal(terminalSessionReason(pcLoginReplaced), pcLoginReplaced); assert.equal(terminalSessionReason(sessionIdleTimeout), sessionIdleTimeout); assert.equal(terminalSessionReason('unknown'), null);
  const f = fixture(), admin = f.add('admin', 'admin'); f.advance(1800000); assert.deepEqual(f.api.sweepIdle(), { ended: [], warnings: [], unavailable: [] }); assert.ok(f.rows.has(admin));
  assert.equal(f.api.describe(tokenHash(999)), null); assert.equal(f.api.enforceCurrent(tokenHash(999)), false);
});

test('missing and asynchronous authority adapters fail closed instead of pretending a commit', () => {
  assert.throws(() => new SessionAccess(), fails('session_access_authority_required'));
  const f = fixture(); f.authority.account = async () => clone(f.user); assert.throws(() => f.api.begin(clone(f.user)), fails('session_access_authority_must_be_synchronous'));
});

test('typed bindings reject coercible array identities and hashes instead of silently canonicalizing peer-like values', () => {
  const f = fixture();
  assert.throws(() => f.api.begin({ ...f.user, id: [f.user.id] }), fails('login_confirmation_stale'));
  const hash = f.add(); f.rows.get(hash).tokenHash = [hash];
  assert.throws(() => f.api.begin(clone(f.user)), fails('session_access_unavailable'));
  const g = fixture(); g.add(); const offer = g.api.begin(clone(g.user)), value = g.next(); value.tokenHash = [value.tokenHash];
  assert.throws(() => g.api.confirm(offer.challenge, value), fails('invalid_session_access_input')); assert.equal(g.counts.commits, 0);
});

test('malformed final idle metadata cannot become a verified expiry receipt', () => {
  const f = fixture(), hash = f.add(); f.advance(1800000);
  f.authority.endIdleCAS = value => { const row = clone(f.rows.get(hash)); row.expiresAt = NaN; assert.equal(value.validateCurrent(row), false); return false; };
  assert.equal(f.api.enforceCurrent(hash), false); assert.ok(f.rows.has(hash)); assert.equal(f.reasons.size, 0);
});

test('every trusted client row requires a strict idleEnabled boolean; no missing or coercible policy default', () => {
  for (const value of [undefined, null, 0, 1, 'false', []]) {
    const f = fixture(), hash = f.add('legacy', 'client', { idleEnabled: value });
    assert.throws(() => f.api.describe(hash), fails('session_activity_unavailable'));
    assert.equal(f.api.enforceCurrent(hash), false); assert.ok(f.rows.has(hash));
  }
  const f = fixture(); delete f.authority.setIdleEnabledCAS;
  assert.throws(() => f.make(), fails('session_access_authority_required'));
});

test('disabled sessions have exactly seven descriptor keys and no countdown, warning or idle expiry including active remote', () => {
  const f = fixture(), hash = f.add('windows-pc-v1', 'client', { idleEnabled: false, attachedRemote: true });
  f.advance(3600000); const descriptor = f.api.describe(hash);
  assert.deepEqual(Object.keys(descriptor).sort(), ['idleDeadline', 'idleEnabled', 'idleTimeoutMs', 'protocol', 'serverTime', 'warningAt', 'warningMs']);
  assert.equal(descriptor.idleEnabled, false); assert.equal(descriptor.idleDeadline, null); assert.equal(descriptor.warningAt, null);
  assert.equal(descriptor.idleTimeoutMs, 1800000); assert.equal(descriptor.warningMs, 60000); assert.equal(descriptor.serverTime, new Date(f.now()).toISOString());
  assert.equal(f.api.enforceCurrent(hash), true); assert.deepEqual(f.api.sweepIdle(), { ended: [], warnings: [], unavailable: [] });
  assert.equal(f.counts.ends, 0); assert.equal(f.rows.get(hash).lastInputAt, null);
});

test('disabled idle policy never bypasses absolute expiry or a confirmed login replacement', () => {
  const f = fixture(), hash = f.add('legacy', 'client', { idleEnabled: false, expiresAt: f.now() + 100 }); f.advance(100);
  assert.equal(f.api.describe(hash), null); assert.equal(f.api.enforceCurrent(hash), false);
  assert.throws(() => f.api.setIdleEnabled(f.input(hash, 'ui'), true), fails('session_expired'));
  const g = fixture(), old = g.add('browser-console', 'client', { idleEnabled: false }), offer = g.api.begin(clone(g.user)), next = g.next();
  assert.equal(offer.kind, 'confirmation-required'); g.api.confirm(offer.challenge, next);
  assert.equal(g.api.enforceCurrent(old), false); assert.equal(g.reasons.get(old).reason, pcLoginReplaced);
  assert.equal(g.rows.get(next.tokenHash).idleEnabled, true);
});

test('real accepted input remains tracked with bounded coalescing while idle logout is off', () => {
  const f = fixture(), hash = f.add('legacy', 'client', { idleEnabled: false }); f.advance(3600000);
  let descriptor = f.api.recordAcceptedInput(f.input(hash));
  assert.equal(f.rows.get(hash).lastInputAt, f.now()); assert.equal(f.counts.persist, 1); assert.equal(descriptor.idleDeadline, null);
  f.advance(1000); descriptor = f.api.recordAcceptedInput(f.input(hash)); assert.equal(f.counts.persist, 1); assert.equal(descriptor.idleEnabled, false);
  f.advance(5000); assert.equal(f.api.enforceCurrent(hash), true); assert.equal(f.counts.persist, 2);
  assert.deepEqual(f.api.sweepIdle(), { ended: [], warnings: [], unavailable: [] });
});

test('an explicit authenticated toggle is immediate durable UI input for this session only; off to on starts fresh 30 minutes', () => {
  const f = fixture(), a = f.add(), b = f.add('android'); f.advance(1000);
  const off = f.api.setIdleEnabled(f.input(a, 'ui'), false);
  assert.equal(off.idleEnabled, false); assert.equal(off.idleDeadline, null); assert.equal(f.rows.get(a).lastInputAt, f.now());
  assert.equal(f.rows.get(b).idleEnabled, true); assert.equal(f.rows.get(b).lastInputAt, null);
  f.advance(3600000); const on = f.api.setIdleEnabled(f.input(a, 'ui'), true);
  assert.equal(on.idleEnabled, true); assert.equal(on.idleDeadline, new Date(f.now() + 1800000).toISOString());
  assert.equal(on.warningAt, new Date(f.now() + 1740000).toISOString()); assert.equal(f.rows.get(a).lastInputAt, f.now()); assert.equal(f.counts.toggles, 2);
  f.advance(1799999); assert.equal(f.api.enforceCurrent(a), true); f.advance(1); assert.equal(f.api.enforceCurrent(a), false);
  assert.equal(f.reasons.get(a).reason, sessionIdleTimeout);
});

test('expired enabled sessions cannot turn idle expiry off or revive it at the exact deadline', () => {
  for (const enabled of [false, true]) {
    const f = fixture(), hash = f.add(); f.advance(1800000);
    assert.throws(() => f.api.setIdleEnabled(f.input(hash, 'ui'), enabled), fails('session_idle_timeout'));
    assert.equal(f.counts.toggles, 0); assert.equal(f.rows.has(hash), false); assert.equal(f.reasons.get(hash).reason, sessionIdleTimeout);
  }
});

test('only single-use server-authenticated UI proof and a strict boolean can toggle, never remote input or peer JSON', () => {
  const f = fixture(), hash = f.add();
  assert.throws(() => f.api.setIdleEnabled({ tokenHash: hash, userId: f.user.id, kind: 'ui' }, false), fails('session_activity_not_admitted'));
  assert.throws(() => f.api.setIdleEnabled(f.input(hash), false), fails('session_activity_not_admitted'));
  assert.throws(() => f.api.setIdleEnabled(f.input(hash, 'ui', { userId: 'different-user' }), false), fails('session_expired'));
  for (const value of [undefined, null, 0, 1, 'false', {}]) assert.throws(() => f.api.setIdleEnabled(f.input(hash, 'ui'), value), fails('invalid_session_access_input'));
  const proof = f.input(hash, 'ui'); f.api.setIdleEnabled(proof, false);
  assert.throws(() => f.api.setIdleEnabled(proof, true), fails('session_activity_not_admitted')); assert.equal(f.counts.toggles, 1);
});

for (const changed of ['token', 'user', 'created', 'mode', 'input', 'tracking-start', 'absolute-expiry', 'missing']) test('fresh toggle transaction rejects changed '+changed+' without updating the successor', () => {
  const f = fixture(), hash = f.add(); f.advance(1000);
  f.hooks.beforeToggle = () => {
    const row = f.rows.get(hash);
    if (changed === 'token') row.tokenHash = tokenHash(999);
    else if (changed === 'user') row.userId = 'different-user';
    else if (changed === 'created') row.createdAt += 1;
    else if (changed === 'mode') row.idleEnabled = false;
    else if (changed === 'input') row.lastInputAt = f.now();
    else if (changed === 'tracking-start') row.activityStartedAt = f.now();
    else if (changed === 'absolute-expiry') row.expiresAt = f.now();
    else f.rows.delete(hash);
  };
  assert.throws(() => f.api.setIdleEnabled(f.input(hash, 'ui'), false), fails('session_access_commit_unverified'));
  assert.equal(f.counts.toggles, 0); assert.equal(f.counts.ends, 0);
});

test('a trusted idle transaction sees concurrent disabling and cannot evict that session', () => {
  const f = fixture(), hash = f.add(); f.advance(1800000);
  f.hooks.beforeEnd = () => { f.rows.get(hash).idleEnabled = false; };
  assert.equal(f.api.enforceCurrent(hash), true); assert.ok(f.rows.has(hash)); assert.equal(f.reasons.size, 0);
});

test('toggle requires an actual transaction guard and durable mode/input readback, not just a true return', () => {
  for (const mode of ['no-guard', 'no-write', 'wrong-mode', 'wrong-input']) {
    const f = fixture(), hash = f.add(); f.advance(1000);
    f.authority.setIdleEnabledCAS = value => {
      if (mode === 'no-guard') return true;
      assert.equal(value.validateCurrent(clone(f.rows.get(hash))), true);
      if (mode === 'wrong-mode') f.rows.get(hash).lastInputAt = value.lastInputAt;
      if (mode === 'wrong-input') f.rows.get(hash).idleEnabled = value.enabled;
      return true;
    };
    assert.throws(() => f.api.setIdleEnabled(f.input(hash, 'ui'), false), fails('session_access_commit_unverified'));
  }
});

test('disable clears warning and dirty timestamp; re-enable gets exactly its own new warning and no stale flush', () => {
  const f = fixture(), hash = f.add(); f.advance(1740000); assert.equal(f.api.sweepIdle().warnings.length, 1);
  f.api.recordAcceptedInput(f.input(hash)); f.advance(1000); f.api.recordAcceptedInput(f.input(hash));
  const dirtyInput = f.now(); f.advance(1000); f.api.setIdleEnabled(f.input(hash, 'ui'), false);
  const durableToggle = f.now(); assert.ok(durableToggle > dirtyInput); assert.equal(f.rows.get(hash).lastInputAt, durableToggle);
  f.advance(1800000); assert.deepEqual(f.api.sweepIdle(), { ended: [], warnings: [], unavailable: [] }); assert.equal(f.rows.get(hash).lastInputAt, durableToggle);
  f.api.setIdleEnabled(f.input(hash, 'ui'), true); f.advance(1740000);
  assert.equal(f.api.sweepIdle().warnings.length, 1); assert.equal(f.api.sweepIdle().warnings.length, 0); assert.ok(f.rows.has(hash));
});
