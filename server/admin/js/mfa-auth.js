// Shared browser MFA boundary. No storage, cookie parsing, clipboard or retries.
import { t } from './i18n.js';
const token = /^[A-Za-z0-9_-]{43}$/;
const record = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const shape = (value, keys) => record(value) && Object.keys(value).length === keys.length && keys.every(key => Object.hasOwn(value, key));
const safeCode = value => typeof value === 'string' && /^[a-z][a-z0-9_]{0,79}$/.test(value) ? value : 'mfa_request_failed';
const texts = {
  invalid_credentials: t('mfa-auth.1'),
  invalid_mfa_fields: t('mfa-auth.2'),
  mfa_invalid_code: t('mfa-auth.3'),
  mfa_challenge_expired: t('common.26'),
  too_many_attempts: t('mfa-auth.4'),
  current_password_incorrect: t('mfa-auth.5'),
  mfa_reauth_required: t('mfa-auth.6'),
  mfa_not_enabled: t('mfa-auth.7'),
  mfa_requires_https: t('mfa-auth.8'),
  mfa_unavailable: t('mfa-auth.9'),
  admin_session_changed: t('mfa-auth.10'),
  mfa_response_invalid: t('mfa-auth.11'),
  mfa_network_unknown: t('mfa-auth.12'),
  authentication_required: t('mfa-auth.13'),
  csrf_required: t('mfa-auth.14'),
};
// The ID and password step of the sign-in: there is no recovery code to offer there, and an ID the server
// cannot read (a space in it) is an ID to check, not a request that failed.
const passwordStepTexts = { too_many_attempts: t('mfa-auth.tooManySignInF3'), invalid_credentials_format: t('mfa-auth.1') };
export class MfaClientError extends Error {
  constructor(status, code, { uncertain = false, passwordStep = false } = {}) {
    const key = safeCode(code);
    super((passwordStep && passwordStepTexts[key]) || texts[key] || t('mfa-auth.15'));
    this.name = 'MfaClientError'; this.status = Number.isInteger(status) ? status : 0; this.code = key; this.uncertain = uncertain;
  }
}
export const mfaClientError = (status, code, options) => new MfaClientError(status, code, options);
// Local privacy deadline only; the server independently enforces its expiry.
export function createMfaDeadline(expiresAt, { now = Date.now, monotonic = () => performance.now() } = {}) {
  const wallAt = now(), monoAt = monotonic(), expiry = Date.parse(expiresAt);
  const duration = Math.min(120000, expiry - wallAt);
  const remaining = () => {
    const wall = now(), mono = monotonic();
    if (![wallAt, monoAt, expiry, duration, wall, mono].every(Number.isFinite) || duration <= 0 || wall < wallAt || mono < monoAt) return 0;
    return Math.max(0, Math.min(expiry - wall, duration - (mono - monoAt)));
  };
  return Object.freeze({ remaining, valid: () => remaining() > 0 });
}
export function normalizeMfaRecoveryCode(value) {
  if (typeof value !== 'string') return null;
  if (/^[a-fA-F0-9]{32}$/.test(value)) return value.toLowerCase();
  if (/^(?:[a-fA-F0-9]{4}-){7}[a-fA-F0-9]{4}$/.test(value)) return value.replaceAll('-', '').toLowerCase();
  return null;
}
export function normalizeMfaCode(value) { return typeof value === 'string' && /^[0-9]{6}$/.test(value) ? value : null; }
export function mfaProofFields(input, needsFactor) {
  if (!record(input) || typeof input.currentPassword !== 'string' || input.currentPassword.length < 1 || input.currentPassword.length > 256) throw new MfaClientError(400, 'invalid_mfa_fields');
  const result = { currentPassword: input.currentPassword };
  const hasCode = input.code !== undefined, hasRecovery = input.recoveryCode !== undefined;
  if (!needsFactor) { if (hasCode || hasRecovery) throw new MfaClientError(400, 'invalid_mfa_fields'); return result; }
  if (hasCode === hasRecovery) throw new MfaClientError(400, 'invalid_mfa_fields');
  const value = hasCode ? normalizeMfaCode(input.code) : normalizeMfaRecoveryCode(input.recoveryCode);
  if (value === null) throw new MfaClientError(400, 'invalid_mfa_fields');
  result[hasCode ? 'code' : 'recoveryCode'] = value; return result;
}
const invalid = () => { throw new MfaClientError(0, 'mfa_response_invalid', { uncertain: true }); };
export function readMfaStatus(body, status) {
  if (status !== 200 || !shape(body, ['protocol', 'enabled', 'recoveryCodesRemaining']) || body.protocol !== 'mfa-v1' || typeof body.enabled !== 'boolean' || !Number.isSafeInteger(body.recoveryCodesRemaining) || body.recoveryCodesRemaining < 0 || body.recoveryCodesRemaining > 10) invalid();
  return { enabled: body.enabled, recoveryCodesRemaining: body.recoveryCodesRemaining };
}
export function readMfaSetup(body, status, now = Date.now()) {
  if (status !== 201 || !shape(body, ['protocol', 'setupToken', 'expiresAt', 'secret', 'uri']) || body.protocol !== 'mfa-v1' || !token.test(body.setupToken) || typeof body.secret !== 'string' || !/^[A-Z2-7]{32}$/.test(body.secret) || typeof body.expiresAt !== 'string') invalid();
  const expiry = Date.parse(body.expiresAt);
  if (!Number.isFinite(expiry) || expiry <= now || expiry > now + 120000) invalid();
  let uri; try { uri = new URL(body.uri); } catch { invalid(); }
  if (uri.protocol !== 'otpauth:' || uri.host !== 'totp' || uri.searchParams.get('secret') !== body.secret || uri.searchParams.get('issuer') !== 'Multitap' || uri.searchParams.get('algorithm') !== 'SHA1' || uri.searchParams.get('digits') !== '6' || uri.searchParams.get('period') !== '30') invalid();
  return { setupToken: body.setupToken, expiresAt: body.expiresAt, secret: body.secret };
}
export function readMfaReauth(body, status) { if (status !== 200 || !shape(body, ['ok']) || body.ok !== true) invalid(); return true; }
export function readMfaChanged(body, status, intent) {
  const codesRequired = intent === 'setup' || intent === 'recovery';
  if (status !== 200 || !shape(body, codesRequired ? ['enabled', 'recoveryCodes', 'requiresLogin'] : ['enabled', 'requiresLogin']) || body.enabled !== (intent !== 'disable') || body.requiresLogin !== true) invalid();
  const codes = codesRequired ? body.recoveryCodes : [];
  if (codesRequired && (!Array.isArray(codes) || codes.length !== 10 || !codes.every(value => typeof value === 'string' && /^[a-f0-9]{32}$/.test(value)) || new Set(codes).size !== 10)) invalid();
  return { enabled: body.enabled, codes: [...codes], requiresLogin: true };
}
export function readAdminMfaChallenge(body, status, now = Date.now()) {
  if (status !== 202 || !shape(body, ['mfa']) || !shape(body.mfa, ['protocol', 'challengeToken', 'expiresAt', 'methods']) || body.mfa.protocol !== 'mfa-v1' || !token.test(body.mfa.challengeToken) || typeof body.mfa.expiresAt !== 'string' || !Array.isArray(body.mfa.methods) || body.mfa.methods.join(',') !== 'totp,recovery') invalid();
  const until = Date.parse(body.mfa.expiresAt);
  if (!Number.isFinite(until) || until <= now || until > now + 120000) invalid();
  return { challengeToken: body.mfa.challengeToken, expiresAt: body.mfa.expiresAt };
}
export function readAdminSession(body, status) {
  if (status !== 200 || !record(body) || body.mfa !== undefined || body.sessionToken !== undefined || !record(body.user) || body.user.role !== 'admin' || typeof body.user.id !== 'string' || !body.user.id || typeof body.user.username !== 'string' || !body.user.username || typeof body.csrfToken !== 'string' || !/^[A-Za-z0-9_-]{1,256}$/.test(body.csrfToken)) invalid();
  return body;
}

// Authentication uses a separate response owner so a canceled request cannot
// expose a session. Cookie-setting late200 cleanup uses ONLY that reply's CSRF;
// a newer cookie rejects the mismatched token on the server. No202/409 logout.
export async function retireMfaCookie({ path, csrfToken, origin, admittedOrigin, fetcher = globalThis.fetch, timeoutMs = 5000, csrfHeader = 'X-CSRF-Token' }) {
  if (origin() !== admittedOrigin || typeof csrfToken !== 'string' || !token.test(csrfToken)) return false;
  const abort = new AbortController(); let timer;
  try {
    return await Promise.race([
      Promise.resolve().then(() => fetcher(path, { method: 'POST', credentials: 'same-origin', headers: { [csrfHeader]: csrfToken }, signal: abort.signal })).then(reply => reply.ok === true, () => false),
      new Promise(resolve => { timer = setTimeout(() => { abort.abort(); resolve(false); }, timeoutMs); }),
    ]);
  } catch { return false; }
  finally { clearTimeout(timer); }
}
export async function requestMfaAuthentication({ path, data, owner, isCurrent, origin, fetcher = globalThis.fetch, logoutPath = '/api/v1/admin/logout', timeoutMs = 15000 }) {
  const admittedOrigin = origin(), current = () => isCurrent(owner) && origin() === admittedOrigin;
  if (!current()) throw new MfaClientError(0, 'admin_session_changed');
  const abort = new AbortController(); let timer, timedOut = false;
  const work = (async () => {
    let response;
    try { response = await fetcher(path, { method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'application/json', 'X-Multitap-Auth-Protocol': 'mfa-v1' }, body: JSON.stringify(data), signal: abort.signal }); }
    catch { return { network: true }; }
    try { return { response, body: await response.json() }; } catch { return { invalid: true }; }
  })();
  const cleanup = value => value.response?.status === 200 ? retireMfaCookie({ path: logoutPath, csrfToken: value.body?.csrfToken, origin, admittedOrigin, fetcher }) : Promise.resolve(false);
  // A late response can still set HttpOnly cookies even after fetch abort or a
  // local timeout. Its response token is the only safe cleanup credential.
  void work.then(value => { if (timedOut) void cleanup(value); });
  let envelope;
  try { envelope = await Promise.race([work, new Promise((_, reject) => { timer = setTimeout(() => { timedOut = true; abort.abort(); reject(new MfaClientError(0, 'mfa_network_unknown', { uncertain: true })); }, timeoutMs); })]); }
  catch (error) { if (!current()) throw new MfaClientError(0, 'admin_session_changed'); throw error; }
  finally { clearTimeout(timer); }
  const { response, body } = envelope;
  if (envelope.network) { if (!current()) throw new MfaClientError(0, 'admin_session_changed'); throw new MfaClientError(0, 'mfa_network_unknown', { uncertain: true }); }
  if (envelope.invalid) { if (!current()) throw new MfaClientError(0, 'admin_session_changed'); invalid(); }
  if (!current()) {
    void cleanup(envelope);
    throw new MfaClientError(0, 'admin_session_changed');
  }
  if (!response.ok) throw new MfaClientError(response.status, body?.error?.code, { passwordStep: data?.challengeToken === undefined });
  return response.status === 202 ? { kind: 'challenge', challenge: readAdminMfaChallenge(body, response.status) } : { kind: 'session', session: readAdminSession(body, response.status) };
}
