import { HttpError } from './errors.mjs';

export const pcSessionPolicy = 'windows-pc-v1';
export const pcLoginReplaced = 'pc_login_replaced';
export const replacedSessionsPerUser = 32, replacedSessionsTotal = 4096;

// Only the public client-login route calls this parser. Headers are an opt-in
// protocol, not hardware attestation or authority over admin/console issuers.
export function clientSessionPolicy(req) {
  const policy = req.headers['x-multitap-session-policy'];
  const intent = req.headers['x-multitap-login-intent'];
  if (policy === undefined && intent === undefined) return { issuer: 'legacy' };
  const count = name => (req.rawHeaders ?? []).filter((value, index) => index % 2 === 0 && value.toLowerCase() === name).length;
  if (count('x-multitap-session-policy') > 1 || count('x-multitap-login-intent') > 1 ||
      policy !== pcSessionPolicy || !['interactive-v1', 'recovery-v1', 'confirmable-v1'].includes(intent)) throw new HttpError(400, 'invalid_session_policy');
  return { issuer: pcSessionPolicy, loginIntent: intent };
}
