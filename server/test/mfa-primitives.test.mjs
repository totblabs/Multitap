// Private integration fixture: synthetic vectors and actual Vault in-memory key.
import test from 'node:test';
import assert from 'node:assert/strict';
import { Vault } from '../src/vault.mjs';
import { totpAt, matchingStep, sealFactor, openFactor, createRecoveryCodes, recoveryDigest } from '../src/mfa-primitives.mjs';
const secret = Buffer.from('12345678901234567890', 'ascii');
const account = '11111111-2222-3333-4444-555555555555';
test('RFC6238 SHA1 vectors, including beyond 32-bit wall time', () => {
  for (const [seconds, expected] of [[59,'94287082'],[1111111109,'07081804'],[1111111111,'14050471'],
    [1234567890,'89005924'],[2000000000,'69279037'],[20000000000,'65353130']]) {
    assert.equal(totpAt(secret, Math.floor(seconds / 30), 8), expected);
  }
});
test('RFC4226 counter profile and fixed six-digit request shape', () => {
  assert.equal(totpAt(secret, 0), '755224'); assert.equal(totpAt(secret, 9), '520489');
  for (const code of ['75522', '7552240', ' 755224', 755224]) assert.throws(() => matchingStep(secret, code, 0));
});
test('window and persisted high-water step reject replay and older/far-future codes', () => {
  const step = 50000000, now = step * 30000;
  assert.equal(matchingStep(secret, totpAt(secret, step), now), step);
  assert.equal(matchingStep(secret, totpAt(secret, step), now, step), null);
  assert.equal(matchingStep(secret, totpAt(secret, step - 2), now), null);
  assert.equal(matchingStep(secret, totpAt(secret, step + 2), now), null);
  assert.equal(matchingStep(secret, totpAt(secret, step + 1), now), step + 1);
});
test('existing Vault ciphertext is opaque and cannot move across account/generation', () => {
  const vault = new Vault(Buffer.alloc(32, 7)), box = sealFactor(vault, account, 2, secret);
  assert.equal(box.includes(secret.toString('hex')), false);
  const opened = openFactor(vault, account, 2, box);
  try { assert.deepEqual(opened, secret); } finally { opened.fill(0); }
  assert.throws(() => openFactor(vault, account, 3, box), /mfa_unavailable/);
  assert.throws(() => openFactor(vault, 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee', 2, box), /mfa_unavailable/);
  assert.throws(() => openFactor(new Vault(Buffer.alloc(32, 8)), account, 2, box), /mfa_unavailable/);
});
test('ten independent 128-bit codes store only identity/generation-bound digests', () => {
  let serial = 0;
  const result = createRecoveryCodes(account, 2, count => Buffer.alloc(count, ++serial));
  assert.equal(new Set(result.codes).size, 10); assert.equal(new Set(result.hashes).size, 10);
  for (const code of result.codes) assert.match(code, /^[a-f0-9]{32}$/);
  assert.equal(recoveryDigest(account, 2, result.codes[0].toUpperCase()), result.hashes[0]);
  assert.notEqual(recoveryDigest(account, 3, result.codes[0]), result.hashes[0]);
  assert.throws(() => createRecoveryCodes(account, 2, count => Buffer.alloc(count)), /mfa_unavailable/);
});
