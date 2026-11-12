# MFA v1 backend contract

Backend contract of the opt-in two-step sign-in. No account is enabled
automatically.

All paths below start with `/api/v1`. MFA operations require the resolved
HTTPS installation origin, with actual direct TLS or an admitted trusted TLS
reverse-proxy hop. An HTTPS address label or spoofed forwarding headers alone
cannot authorize MFA operations.
Existing MFA-OFF login and internal Host HTTP flows retain their old behavior.
Responses containing authentication or enrollment data are `no-store`.
Never place challenge tokens, OTPs or recovery codes in an HTTP URL or log.

## Login

`POST /auth/login` and `POST /admin/login` keep `{username,password}` unchanged.
New clients send `X-Multitap-Auth-Protocol: mfa-v1`. Old brokers ignore that header.

- MFA OFF: the existing HTTP200 response is unchanged, including admin cookie.
- MFA ON without support header: HTTP428 `mfa_client_update_required`, no session.
- MFA ON with header: HTTP202
  `{mfa:{protocol:"mfa-v1",challengeToken,expiresAt,methods:["totp","recovery"]}}`.
  This response has no session token, administrator cookie or CSRF grant.
- `POST /auth/mfa/verify` (client) or `/admin/mfa/verify` (admin):
  `{challengeToken,code}` OR `{challengeToken,recoveryCode}`, mutually exclusive.
  Success returns the exact normal login HTTP200 body and only then issues a
  session/admin cookie. A challenge cannot cross those purposes or origins.

The browser-console controller uses the server-only provider hooks
`login(input,false,'console')` and `mfaRoutes.verify(input,false,'console')`.
That challenge audience is distinct. Its internal grant uses the existing
`client` session kind; only the console controller receives the raw grant and
must set its separate HttpOnly cookie without exposing bearer JSON. No console
bearer endpoint is exposed. Its endpoints are `/console/api/login` and
`/console/api/mfa/verify`; the latter additionally requires a challenge-scoped
CSRF token returned by202. The shared password login header is unchanged.

Challenges last 120 seconds, permit at most 5 failed attempts and are bounded in
memory. They bind immutable user ID, password state and MFA generation. Account MFA-code
failure limits survive new challenges and broker restarts: 5 failures per 60s,
then a 60s cooldown. Existing address/password limits also remain in force.
An invalid code returns HTTP400 `mfa_invalid_code`; expired/used challenges are
HTTP410 `mfa_challenge_expired`; throttling is HTTP429 `too_many_attempts`.
A monotonic deadline prevents wall-clock rollback extending a live challenge.
Account authorization versions also invalidate challenges after temporary
disable/enable or demote/promote cycles; password/factor changes remain bound.

## Account management

Client bearer routes use `/auth/mfa`; administrator cookie routes use
`/admin/mfa` with the same suffix/body and the existing Origin/CSRF checks.
These routes manage only the authenticated account, never another user.
The browser-cookie equivalents are `/console/api/mfa` and the same suffixes,
with console-scoped Origin/CSRF/session checks. `/console/account/security`
opens self-service without a VM assignment and never creates a console grant.

| Method and suffix | Body | Success |
| --- | --- | --- |
| GET root | none | `{protocol:"mfa-v1",enabled,recoveryCodesRemaining}` |
| POST `/setup` | `{currentPassword}` | HTTP201 `{protocol:"mfa-v1",setupToken,expiresAt,secret,uri}` |
| POST `/enable` | `{setupToken,code}` | `{enabled:true,recoveryCodes,requiresLogin:true}` |
| POST `/reauth` | `{currentPassword,code}` OR `{currentPassword,recoveryCode}` | `{ok:true}` |
| POST `/disable` | `{currentPassword}` | `{enabled:false,requiresLogin:true}` |
| POST `/recovery` | `{currentPassword}` | `{enabled:true,recoveryCodes,requiresLogin:true}` |

Setup requires password reauthentication. Replacing an enabled factor,
disabling it or regenerating recovery codes also requires a successful MFA
verification within 5 minutes on the same current session and factor generation;
otherwise HTTP403 `mfa_reauth_required`. `/reauth` provides that verification.
Setup tokens are 120s, session/origin/purpose/password/generation bound. Setup
does not enable MFA: `/enable` must verify the matching candidate TOTP first.
Enrollment consumes that step. An immediate new login must use the next code;
the UI must explain this instead of weakening one-time validation.
Ten independent 128-bit recovery codes are returned once; only their hashes
are stored. Factor changes revoke all prior sessions and pending authority.
Normal password reset preserves the existing MFA factor and recovery state.

TOTP uses SHA1, 6 digits, 30s, 20 random bytes and current step±1. Successful steps
are consumed persistently; replay and recovery consumption share the transaction
that reauthorizes and issues/updates the session. Vault ciphertext contains
`{kind:"totp-v1",userId,generation,secret}` and is validated after decryption.
The API never returns an active factor secret through status or user inventory.
The URI follows the local `otpauth://totp/` convention, never an external QR URL.

## Compatibility and recovery

Keep `BrokerApi.login()` returning a normal session. Windows/Android
`HttpBrokerApi.login()` can turn the negotiated 202 body into a typed
`MfaRequiredException` carrying only the short-lived challenge. Silent password
login must stop for interactive OTP, retain a valid remembered password, and
never remember OTPs, challenge tokens, setup secrets or recovery codes.
Admin login must branch on202 before entering its normal signed-in screen.

The migration is schema 17. Existing users stay MFA OFF. A broker older than
schema 17 rejects the database; a rollback must restore the validated
pre-migration database with its matching Vault key. Full disaster backup also needs protected installation configuration.
Server-console root recovery uses `mfa-reset <UUID> --confirm-user-id <UUID>`;
both UUIDs must match, the exact account is audited and its sessions are revoked.
There is no generic remote administrator MFA-reset route.

Algorithm/reference tests: [RFC6238](https://www.rfc-editor.org/rfc/rfc6238.txt),
[RFC4226](https://www.rfc-editor.org/rfc/rfc4226.txt). Enrollment URI convention:
[Google Key URI format](https://github.com/google/google-authenticator/wiki/Key-Uri-Format).

New fixed errors: HTTP403 `mfa_requires_https` / `mfa_reauth_required`;
HTTP409 `mfa_not_enabled`; HTTP503 `mfa_unavailable`; HTTP400
`invalid_mfa_fields`. Existing password, CSRF, Origin, field and session errors
retain their existing meanings. No response echoes an OTP or recovery code
except the explicit once-only recovery-code creation response.
