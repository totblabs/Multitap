# Administrator mutation reauthorization

Administrator requests in `server/src/admin-http.mjs` often wait before they change anything: for the request body, for a password hash job (`passwordJob`) or for a Proxmox or Host call. The administrator who sent the request may lose that authority during the wait (demoted, disabled, logged out, session expired, password reset, credentials change required). The authority checked when the request arrived is therefore not used for the mutation.

## Rule

Each route authenticates the admin cookie, the role and (for writes) the exact `Origin` and CSRF token before it reads the body. After every wait and immediately before the mutation, it calls `currentAdmin()`, which authenticates the same cookie against the store again and requires:

- the same immutable account ID as the original request (a deleted and recreated account with the same login ID does not match);
- the `admin` role, an enabled account and an unexpired admin session;
- no pending credential change, except on the first-setup route (`/api/v1/admin/setup`) and logout;
- a valid CSRF token for the current session.

If any check fails the request ends with `authentication_required` (401) or `csrf_required` (403) and nothing is written: no account, assignment, setting, enrolment or audit row.

The store's own guards (primary administrator, last administrator, sessions removed on demotion or disablement) still apply. They protect the target account; `currentAdmin()` protects against a stale actor.

## Coverage

The check runs after the body or password wait on: account create, update, enable/disable, password reset and delete; first setup; assignment batch and per-VM permission updates; desktop display edits and local PC renames; diagnostic collection requests; Proxmox connection save, sync and task reads; server access test and save; server update check and apply; the launcher preview switch; Host request approval and rejection; Host updates; and power actions.

Some operations pass `currentAdmin` as a callback so the check happens later still:

- Host approval, Host update and power actions call it just before they commit their own state.
- The legacy enrolment-code route (`POST /api/v1/admin/desktops/:id/enrollment`, kept for old Hosts) passes it to `Relay.createEnrollment`, which calls it after the Proxmox identity lookup and immediately before issuing a code.

An operation that was already accepted (a power task or Host update in progress) still records its completion and audit rows after the administrator logs out. Only new mutations require current authority.

## Tests

`server/test/admin-reauthorization.test.mjs` drives the real router and an in-memory store with a real admin session cookie and CSRF token. Each mutation route is paused at its body, password job or identity lookup while the actor is demoted, disabled, logged out, expired, given a password reset or a required credential change, has its CSRF changed, or is deleted and recreated. The test then checks that the database is unchanged, and that the same routes still succeed for a current administrator, for first setup and for an already accepted operation.
