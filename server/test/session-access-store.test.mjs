import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { SessionAccess } from '../src/session-access.mjs';
import { createSessionAccessStore } from '../src/session-access-store.mjs';

for (const disabledCount of [0, 1, 1024]) test(`periodic idle cleanup reaches due enabled owners behind ${disabledCount} disabled owners`, () => {
  const store = new Store(':memory:'); let now = Date.UTC(2026, 10, 5, 7);
  try {
    const adapter = createSessionAccessStore(store, { now: () => now });
    const access = new SessionAccess({ authority: adapter.authority, now: () => now,
      policy: { idleTimeoutMs: 60000, warningMs: 10000, ...(disabledCount < 1024 ? { sweepBatch: 1 } : {}) } });
    const add = (i, disabled) => {
      const name = `sweep-fixture-${i}`, userId = store.addUser({ name, passwordHash: 'synthetic-unused-hash' });
      const hash = createHash('sha256').update(name).digest('hex');
      store.createSession(store.findUser(name), hash, now, 3600000, 'client'); access.describe(hash);
      if (disabled) access.setIdleEnabled(adapter.mintInputProof(hash, userId, 'ui'), false);
      return { hash, userId };
    };
    const disabled = Array.from({ length: disabledCount }, (_, i) => add(i, true));
    now += 1000; const due = add(disabledCount, false); now += 60001;
    assert.deepEqual(access.sweepIdle().ended.map(value => value.tokenHash), [due.hash]);
    assert.equal(store.db.prepare('SELECT 1 FROM sessions WHERE token_hash=?').get(due.hash), undefined);
    assert.equal(store.sessionRevocationReason(due.hash, now), 'session_idle_timeout');
    for (const owner of disabled) assert.equal(access.enforceCurrent(owner.hash), true);
    assert.equal(access.sweepIdle().ended.length, 0);
    if (disabled.length) {
      const owner = disabled[0]; access.setIdleEnabled(adapter.mintInputProof(owner.hash, owner.userId, 'ui'), true);
      assert.equal(access.sweepIdle().ended.length, 0, 'enabling starts a fresh acknowledged interval');
      now += 60001; assert.deepEqual(access.sweepIdle().ended.map(value => value.tokenHash), [owner.hash]);
      for (const other of disabled.slice(1)) assert.equal(access.enforceCurrent(other.hash), true);
    }
  } finally { store.close(); }
});
