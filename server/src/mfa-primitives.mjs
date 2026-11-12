// PRIVATE integration primitives: no session admission or OS/network IO here.
// Production profile is docs/mfa-v1.md: SHA1, 20-byte key, 6 digits, 30 seconds.
import { createHash, createHmac, randomBytes, timingSafeEqual } from 'node:crypto';

const userPattern = /^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/;
const validGeneration = value => Number.isSafeInteger(value) && value > 0;
const shape = (value, keys) => value !== null && typeof value === 'object' &&
  !Array.isArray(value) && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
function binding(userId, generation) {
  if (typeof userId !== 'string' || !userPattern.test(userId) || !validGeneration(generation)) throw Error('mfa_unavailable');
}
function secretBytes(secret) {
  if (!Buffer.isBuffer(secret) || secret.length !== 20) throw Error('mfa_unavailable');
}

// Eight digits is exposed solely for RFC6238 vector testing. The HTTP verifier
// must call matchingStep(), whose production code shape is fixed to six digits.
export function totpAt(secret, step, digits = 6) {
  secretBytes(secret);
  if (!Number.isSafeInteger(step) || step < 0 || ![6, 8].includes(digits)) throw Error('invalid_mfa_fields');
  const counter = Buffer.alloc(8);
  counter.writeBigUInt64BE(BigInt(step));
  const digest = createHmac('sha1', secret).update(counter).digest();
  try {
    const offset = digest[digest.length - 1] & 15;
    const value = digest.readUInt32BE(offset) & 0x7fffffff;
    return String(value % 10 ** digits).padStart(digits, '0');
  } finally { counter.fill(0); digest.fill(0); }
}

// Returns the exact counter to compare-and-consume in the SAME Store transaction
// as the server-owned admission/pending-confirmation result. Never consumes here.
export function matchingStep(secret, code, wallMs, lastAcceptedStep = -1) {
  secretBytes(secret);
  if (typeof code !== 'string' || !/^[0-9]{6}$/.test(code) ||
      !Number.isSafeInteger(wallMs) || wallMs < 0 ||
      !Number.isSafeInteger(lastAcceptedStep) || lastAcceptedStep < -1) throw Error('invalid_mfa_fields');
  const current = Math.floor(wallMs / 30000), supplied = Buffer.from(code, 'ascii');
  let found = null;
  try {
    for (const step of [current - 1, current, current + 1]) {
      const candidate = Buffer.from(totpAt(secret, Math.max(0, step)), 'ascii');
      try {
        const same = timingSafeEqual(candidate, supplied);
        if (same && step >= 0 && step > lastAcceptedStep) found = step;
      } finally { candidate.fill(0); }
    }
    return found;
  } finally { supplied.fill(0); }
}

// Reuse the actual protected installation Vault key; do not create a new key or
// call Vault.open() from an HTTP request. Row identity is verified after decrypt.
export function sealFactor(vault, userId, generation, secret) {
  binding(userId, generation); secretBytes(secret);
  if (typeof vault?.seal !== 'function') throw Error('mfa_unavailable');
  return vault.seal({ kind: 'totp-v1', userId, generation, secret: secret.toString('hex') });
}
export function openFactor(vault, userId, generation, ciphertext) {
  binding(userId, generation);
  if (typeof vault?.open !== 'function' || typeof ciphertext !== 'string' || ciphertext.length > 4096) throw Error('mfa_unavailable');
  let value;
  try { value = vault.open(ciphertext); }
  catch { throw Error('mfa_unavailable'); }
  if (!shape(value, ['kind', 'userId', 'generation', 'secret']) || value.kind !== 'totp-v1' ||
      value.userId !== userId || value.generation !== generation ||
      typeof value.secret !== 'string' || !/^[a-f0-9]{40}$/.test(value.secret)) throw Error('mfa_unavailable');
  const secret = Buffer.from(value.secret, 'hex');
  value.secret = '';
  return secret; // Caller zeroes this Buffer in finally, including every error.
}

function normalizedRecovery(value) {
  if (typeof value !== 'string' || value.length > 39 || !/^[a-fA-F0-9-]+$/.test(value)) throw Error('invalid_mfa_fields');
  const code = value.replaceAll('-', '').toLowerCase();
  if (!/^[a-f0-9]{32}$/.test(code)) throw Error('invalid_mfa_fields');
  return code;
}
export function recoveryDigest(userId, generation, value) {
  binding(userId, generation);
  return createHash('sha256').update('multitap-mfa-recovery-v1\0').update(userId).update('\0')
    .update(String(generation)).update('\0').update(normalizedRecovery(value)).digest('hex');
}
export function createRecoveryCodes(userId, generation, entropy = randomBytes) {
  binding(userId, generation);
  const codes = [], seen = new Set();
  // Bounded collision handling; never silently issue duplicate codes.
  for (let attempt = 0; attempt < 40 && codes.length < 10; attempt++) {
    const bytes = entropy(16);
    if (!Buffer.isBuffer(bytes) || bytes.length !== 16) throw Error('mfa_unavailable');
    const code = bytes.toString('hex'); bytes.fill(0);
    if (seen.has(code)) continue;
    seen.add(code); codes.push(code);
  }
  if (codes.length !== 10) throw Error('mfa_unavailable');
  return { codes, hashes: codes.map(code => recoveryDigest(userId, generation, code)) };
}
