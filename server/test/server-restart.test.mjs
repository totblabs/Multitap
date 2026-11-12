// 서버 다시 시작 from the console: the admin API on the real server, and the
// 서버 정보 page with injected answers. The restart itself (the process ending
// with the entrypoint's exit code and being started again) needs a container
// and is not run here: `restartServer` is the function index.mjs gives.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes, randomUUID } from 'node:crypto';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer, VERSION } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { loadAdmin, ownerFixture } from './admin-ui-harness.mjs';

const origin = 'https://restart-tests.example', password = 'Restart-tests-only-password-48!';
async function setup(t, options = {}) {
  const store = new Store(':memory:'), hash = await hashPassword(password);
  store.addUser({ name: 'admin', displayName: 'Admin', passwordHash: hash, role: 'admin' });
  store.addUser({ name: 'alice', displayName: 'Alice', passwordHash: hash });
  store.saveSetting('proxmox_initialized', '1', 'protocol-test-fixture');
  const server = await buildServer({ store, vault: new Vault(randomBytes(32)), publicOrigin: origin, logger: { error: value => assert.fail(value) }, ...options });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  let cookie = '', csrf = '';
  const call = async (path, method = 'GET', data, headers = {}) => {
    const response = await fetch(`http://127.0.0.1:${server.address().port}/api/v1/admin` + path, { method, headers: { Cookie: cookie, ...(data !== undefined ? { 'Content-Type': 'application/json' } : {}), ...(method !== 'GET' ? { Origin: origin, 'X-CSRF-Token': csrf } : {}), ...headers }, body: data === undefined ? undefined : JSON.stringify(data) });
    return { status: response.status, headers: response.headers, body: await response.json() };
  };
  const login = async () => { const result = await call('/login', 'POST', { username: 'admin', password }); assert.equal(result.status, 200); cookie = result.headers.get('set-cookie').split(';')[0]; csrf = result.body.csrfToken; };
  return { store, server, call, login };
}
const audits = store => store.db.prepare("SELECT event, actor, subject FROM audit_events WHERE event='server_restart_requested'").all();

test('An administrator restarts a server that can start itself again: confirmed, recorded, answered, then asked to end', async t => {
  let restarts = 0;
  const f = await setup(t, { deploymentKind: 'docker', restartServer: () => { restarts++; } });
  assert.equal((await f.call('/server-restart', 'POST', { confirm: true })).status, 401, 'not without an administrator');
  await f.login();
  const info = await f.call('/server-info');
  assert.deepEqual(info.body.restart, { supported: true }); assert.ok(Number.isFinite(Date.parse(info.body.startedAt))); assert.equal(info.body.version, VERSION);
  assert.equal((await f.call('/server-info')).body.startedAt, info.body.startedAt, 'the same process, the same start');
  // What must be there, and nothing else.
  assert.equal((await f.call('/server-restart')).status, 405);
  for (const body of [{}, { confirm: false }, { confirm: 'yes' }]) assert.deepEqual(await f.call('/server-restart', 'POST', body).then(result => [result.status, result.body.error.code]), [400, 'server_restart_confirmation_required']);
  assert.equal((await f.call('/server-restart', 'POST', { confirm: true, extra: 1 })).status, 400);
  assert.equal((await f.call('/server-restart', 'POST', { confirm: true }, { 'X-CSRF-Token': '' })).status, 403);
  assert.equal((await f.call('/server-restart', 'POST', { confirm: true }, { Origin: 'https://evil.example' })).status, 403);
  assert.equal(restarts, 0); assert.deepEqual(audits(f.store), []);

  // While a change of the access settings holds the fence, the server is not restarted under it.
  const lease = f.server.accessRuntime.lease = { token: 'a'.repeat(64), operationId: randomUUID() };
  assert.deepEqual(await f.call('/server-restart', 'POST', { confirm: true }).then(result => [result.status, result.body.error.code]), [409, 'server_access_pending']);
  f.server.accessRuntime.release(lease);
  assert.equal(restarts, 0);

  const accepted = await f.call('/server-restart', 'POST', { confirm: true });
  assert.equal(accepted.status, 202); assert.deepEqual(accepted.body, { accepted: true, startedAt: info.body.startedAt });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(restarts, 1, 'after the answer');
  const rows = audits(f.store);
  assert.equal(rows.length, 1); assert.equal(rows[0].actor, f.store.findUser('admin').id); assert.equal(rows[0].subject, '');
});

test('An install where ending the server would not start it again offers no restart', async t => {
  for (const deploymentKind of ['systemd', 'docker', 'unknown']) {
    const f = await setup(t, { deploymentKind });
    await f.login();
    assert.deepEqual((await f.call('/server-info')).body.restart, { supported: false }, deploymentKind);
    assert.deepEqual(await f.call('/server-restart', 'POST', { confirm: true }).then(result => [result.status, result.body.error.code]), [503, 'server_restart_unavailable'], deploymentKind);
    assert.deepEqual(audits(f.store), []);
  }
});

// The page: 서버 정보.
const info = (extra = {}) => ({ service: 'multitap-broker', version: '1.2.0', deployment: { kind: 'docker', configured: true }, update: { state: 'not_configured', feedConfigured: false, applySupported: false }, checkedAt: '2026-11-12T10:00:00.000Z', startedAt: '2026-11-12T09:00:00.000Z', restart: { supported: true }, ...extra });
test('서버 정보 offers 서버 다시 시작 only where the server says it can, asks first, and comes back when a later start answers', async t => {
  const { user, inventory } = ownerFixture();
  let answer = info(), down = false;
  const f = await loadAdmin(t, 'server-restart', user, inventory, { deploymentKind: 'docker', hash: '#/server-info', respond: async url => {
    if (url.endsWith('/server-info')) return down ? { ok: false, status: 503, json: async () => ({ error: { code: 'server_restarting' } }) } : { ok: true, status: 200, json: async () => answer };
    if (url.endsWith('/server-restart')) return { ok: true, status: 202, json: async () => ({ accepted: true, startedAt: answer.startedAt }) };
  } });
  t.mock.timers.enable({ apis: ['setTimeout'] });
  await f.settle();
  const button = () => f.content.querySelector('[data-server-restart]');
  assert.equal(button().textContent, '서버 다시 시작'); assert.equal(button().disabled, false);
  f.click(f.content, '서버 다시 시작'); await f.settle();
  assert.equal(f.dialog.open, true); assert.match(f.dialog.textContent, /서버를 다시 시작할까요\?/); assert.match(f.dialog.textContent, /접속 중인 연결이 모두 끊겨요/); assert.deepEqual(f.writes, []);
  f.click(f.dialog, '취소'); await f.settle(); assert.deepEqual(f.writes, []);
  f.click(f.content, '서버 다시 시작'); await f.settle();
  down = true;
  await document.querySelector('#modal-form').onsubmit({ preventDefault() {} }); await f.settle();
  assert.deepEqual(f.writes, [{ path: '/api/v1/admin/server-restart', data: { confirm: true } }]); assert.equal(f.dialog.open, false);
  // It waits: no second request, the reason on the page, no red error for the answers that fail meanwhile.
  assert.equal(button().disabled, true);
  assert.equal(f.content.querySelector('[data-server-restarting]').textContent, '서버를 다시 시작하고 있어요. 응답하면 이 화면이 돌아와요.');
  f.click(f.content, '서버 다시 시작'); await f.settle(); assert.equal(f.writes.length, 1);
  t.mock.timers.tick(1500); await f.settle();
  assert.ok(f.content.querySelector('[data-server-restarting]'), 'still waiting while nothing answers');
  // The old process answering once more is not the restart.
  down = false; t.mock.timers.tick(1500); await f.settle();
  assert.ok(f.content.querySelector('[data-server-restarting]'));
  answer = info({ startedAt: '2026-11-12T10:00:30.000Z' }); t.mock.timers.tick(1500); await f.settle();
  assert.equal(f.content.querySelector('[data-server-restarting]'), null); assert.equal(button().disabled, false);
  assert.match(document.querySelector('#notice').textContent, /서버가 다시 시작됐어요/);
  const reads = f.requests.filter(path => path.endsWith('/server-info')).length;
  t.mock.timers.tick(5000); await f.settle();
  assert.equal(f.requests.filter(path => path.endsWith('/server-info')).length, reads, 'nothing is read once it is back');
});

test('서버 정보 shows no restart on an install that has none', async t => {
  const { user, inventory } = ownerFixture();
  for (const answer of [info({ restart: { supported: false } }), info({ restart: undefined, deployment: { kind: 'systemd', configured: true } })]) await t.test(answer.deployment.kind, async t => {
    const f = await loadAdmin(t, 'server-restart-none-' + answer.deployment.kind, user, inventory, { deploymentKind: answer.deployment.kind, hash: '#/server-info', respond: async url => url.endsWith('/server-info') ? { ok: true, status: 200, json: async () => answer } : null });
    await f.settle();
    assert.equal(f.content.querySelector('[data-server-restart]'), null);
    assert.equal(f.content.textContent.includes('서버 다시 시작'), false);
    assert.ok(f.content.textContent.includes('업데이트 화면으로'));
  });
});
