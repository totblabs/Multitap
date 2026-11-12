import { createHash, randomBytes, timingSafeEqual } from 'node:crypto';

// Short human-comparable codes in Crockford base32 (no I, L, O or U).
const alphabet = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';
export function crockford(bytes, length) {
  let bits = 0, count = 0, text = '';
  for (const byte of bytes) {
    bits = ((bits << 8) | byte) & 0xffff; count += 8;
    while (count >= 5 && text.length < length) { count -= 5; text += alphabet[(bits >> count) & 31]; }
    if (text.length === length) return text;
  }
  throw new Error('code_source_too_short');
}

// Host enrollment verification code (host-request-code-v1). The Host shows it
// in its settings window and the console shows it next to the request; the
// administrator compares the two before approving. Derived from the stored
// request-key hash (hex SHA-256 of requestKey), so no new secret is stored:
//   code = Crockford(SHA-256("multitap-host-request-code-v1:" + keyHashHex))[0..8)
// Keep identical to apps/windows_host/src/pending_connection.hpp.
export const hostRequestCodeProtocol = 'host-request-code-v1';
export function hostRequestCode(keyHash) {
  if (typeof keyHash !== 'string' || !/^[a-f0-9]{64}$/.test(keyHash)) throw new Error('invalid_request_key_hash');
  const text = crockford(createHash('sha256').update(hostRequestCodeProtocol + ':' + keyHash).digest(), 8);
  return text.slice(0, 4) + '-' + text.slice(4);
}

// One-time first-administrator setup code: 12 characters (60 random bits),
// printed only to the server's own log; only its hash is stored.
export function newSetupCode() {
  const text = crockford(randomBytes(8), 12);
  return `${text.slice(0, 4)}-${text.slice(4, 8)}-${text.slice(8)}`;
}
// Accepts the printed form with or without hyphens/spaces and lower case;
// Crockford reads O as 0 and I/L as 1. Anything else is not a code.
export function normalizeSetupCode(value) {
  if (typeof value !== 'string' || value.length > 32) return null;
  const text = value.toUpperCase().replace(/[\s-]/g, '').replace(/O/g, '0').replace(/[IL]/g, '1');
  return /^[0-9A-HJKMNP-TV-Z]{12}$/.test(text) ? text : null;
}
export const setupCodeHash = value => createHash('sha256').update('multitap-setup-code-v1:' + value).digest('hex');
export function setupCodeMatches(value, storedHash) {
  const normalized = normalizeSetupCode(value);
  if (!normalized || typeof storedHash !== 'string' || !/^[a-f0-9]{64}$/.test(storedHash)) return false;
  return timingSafeEqual(Buffer.from(setupCodeHash(normalized), 'hex'), Buffer.from(storedHash, 'hex'));
}
