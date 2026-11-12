import test from 'node:test';
import assert from 'node:assert/strict';
import { createHash, randomUUID } from 'node:crypto';
import { Relay } from '../src/relay.mjs';
import { ConsolePreparation } from '../src/console-preparation.mjs';
import { HostUpdates } from '../src/host-update.mjs';
import { AccessRuntime } from '../src/access-runtime.mjs';
import { adminRoutes } from '../src/admin-http.mjs';

// New synthetic admission-boundary checks. No sockets, operating-system
// mutation, deployment, credentials, input injection or display operation.
const pending = () => new AccessRuntime({ initialOperationId: randomUUID() });
const blocked = error => error?.status === 409 && error?.code === 'server_access_pending';
// A fence the process started with: it was just restarted by an apply, and
// says so the way a restarting server does (access-runtime.mjs).
const restarting = error => error?.status === 503 && error?.code === 'server_restarting';

function fixture(runtime) {
  const messages = [], desktopId = 'pve-4187';
  const host = { identity: { id: 'synthetic-host' }, tokenHash: 'synthetic-hash', ws: {} };
  const relay = Object.create(Relay.prototype);
  Object.assign(relay, {
    assertAccessAdmission: () => runtime.assertAdmission(),
    authorized: () => true,
    sessionStop: { guard: () => {} },
    hosts: new Map([[desktopId, host]]),
    displaySupported: () => true,
    powerBusy: () => false,
    displayBusy: new Map(), displayOperations: new Map(), displayLatest: new Map(), sessions: new Map(),
    hostUpdate: { operations: new Map(), guard: HostUpdates.prototype.guard },
    displayReadyMs: 10000,
    send: (_socket, value) => { messages.push(value); return true; },
    dropHost: () => assert.fail('synthetic relay must not drop a Host'),
    displayView: (_id, _user, operation) => ({ id: operation.id, status: operation.status }),
    store: { audit: () => {}, authenticateHost: () => host.identity },
  });
  return { relay, messages, desktopId };
}

test('display mutation rechecks admission after an HTTP body wait', t => {
  const runtime = pending(); t.after(() => runtime.close());
  const f = fixture(runtime);
  assert.throws(() => f.relay.configureDisplay({ id: 'synthetic-user' }, 'synthetic-hash', f.desktopId, 1), restarting);
  assert.equal(f.messages.length, 0);
  assert.equal(f.relay.displayBusy.size, 0);
});

test('console preparation rechecks admission after an HTTP body wait', t => {
  const runtime = pending(); t.after(() => runtime.close());
  const f = fixture(runtime), preparation = new ConsolePreparation(f.relay);
  preparation.available = () => true; // synthetic Host is ready; test only admission
  assert.throws(() => preparation.start({ id: 'synthetic-user' }, 'synthetic-hash', f.desktopId, 'resume-windows-session'), restarting);
  assert.equal(f.messages.length, 0);
  assert.equal(f.relay.displayBusy.size, 0);
});

test('administrator update apply rechecks admission when its body arrives after a fence', async t => {
  const runtime = new AccessRuntime({}); t.after(() => runtime.close());
  const origin = 'https://synthetic-fixture.invalid', token = 'A'.repeat(43);
  const actor = { id: 'synthetic-admin', role: 'admin', must_change_credentials: false };
  let bodyDone, bodyStarted, applied = 0;
  const started = new Promise(resolve => { bodyStarted = resolve; });
  const body = new Promise(resolve => { bodyDone = resolve; });
  const scope = createHash('sha256').update(origin).digest('hex').slice(0, 16);
  const route = adminRoutes({
    publicOrigin: origin, cookieScope: origin, accessRuntime: runtime,
    store: { authenticate: () => actor, onboarding: () => ({ complete: true }), audit: () => {} },
    vault: { verifyCsrf: () => true },
    body: () => { bodyStarted(); return body; },
    serverInfo: { version: '0.5.84' },
    serverUpdate: { apply: async () => { applied++; return { accepted: true }; } },
  });
  const request = { method: 'POST', headers: { origin, cookie: '__Secure-multitap-admin-' + scope + '=' + token } };
  const response = route(request, {}, '/api/v1/admin/server-update/apply', () => {});
  const rejected = assert.rejects(response, blocked);
  await started;
  runtime.lease = { token: 'f'.repeat(64), operationId: randomUUID() };
  bodyDone({ releaseId: 'b'.repeat(64), fromVersion: '0.5.84', confirm: true });
  await rejected;
  assert.equal(applied, 0);
});
