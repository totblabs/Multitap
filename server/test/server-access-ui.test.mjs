import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { el } from '../admin/js/dom.js';
import { render, validResponse } from '../admin/js/server-access.js';

// Actual admin DOM/controller with injected API answers only. No listener,
// TLS key, network, updater, clipboard or browser navigation is executed.
const rev = 'a'.repeat(64), nextRev = 'b'.repeat(64);
const config = () => ({ protocol: 'server-access-v1', primaryOrigin: 'https://broker.fixture', https: {
  enabled: true, origin: 'https://broker.fixture', mode: 'proxy', listenHost: '127.0.0.1', listenPort: 18400, certFile: null, keyFile: null,
}, http: { enabled: false, origin: null, listenHost: '0.0.0.0', listenPort: 18480 }, trustedProxies: ['127.0.0.1'] });
const response = (extra = {}) => ({ protocol: 'server-access-v1', revision: rev, applied: config(), pending: null,
  managementOrigin: 'https://console.fixture', ...extra });
const flush = async () => { await new Promise(resolve => setImmediate(resolve)); await new Promise(resolve => setImmediate(resolve)); };
const gate = () => { let resolve; const promise = new Promise(done => { resolve = done; }); return { promise, resolve }; };

function fixture(t, { snapshot = response(), handler } = {}) {
  const window = parseHTML('<!doctype html><html><body></body></html>');
  const previous = Object.fromEntries(['document', 'Node'].map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  Object.defineProperty(globalThis, 'document', { configurable: true, value: window.document });
  Object.defineProperty(globalThis, 'Node', { configurable: true, value: window.Node });
  const writes = [], dialogs = [], loads = [];
  let clock = 1000000;
  const state = { user: { id: 'fixture-admin' }, ui: {} };
  const ctx = { state, now: () => clock, setupRequired: () => false, link: (text, page) => el('a', { href: '#/' + page }, text),
    resource: () => ({ value: snapshot, error: '', loading: false }), value: () => snapshot,
    pageHeader: ({ title, description }) => el('header', {}, el('h1', {}, title), el('p', {}, description)),
    render: () => window.document.body.replaceChildren(...render(ctx)),
    api: async (path, method, body) => {
      writes.push({ path, method, body: JSON.parse(JSON.stringify(body)) });
      if (handler) return handler(path, body, ctx);
      return path.endsWith('/test') ? { testId: 'fixture-test', revision: snapshot.revision, ok: true,
        checks: [{ id: 'configuration', ok: true }], expiresAt: clock + 60000 }
        : { operationId: 'fixture-operation', state: 'waiting_idle' };
    },
    modal: (title, fields, action, options) => dialogs.push({ title, fields, action, options }),
    load: async (key, options) => { loads.push({ key, options }); },
  };
  const timers = new Set();
  const redraw = () => { if (state.ui.serverAccess?.expiryTimer) timers.add(state.ui.serverAccess.expiryTimer); ctx.render(); };
  redraw();
  t.after(() => {
    timers.add(state.ui.serverAccess?.expiryTimer);
    for (const timer of timers) clearTimeout(timer);
    for (const [key, descriptor] of Object.entries(previous)) {
      if (descriptor) Object.defineProperty(globalThis, key, descriptor); else delete globalThis[key];
    }
  });
  const find = selector => window.document.querySelector(selector);
  const change = (name, value) => {
    const input = find('[name="' + name + '"]'); assert.ok(input, name);
    if (input.type === 'checkbox') input.checked = value;
    else if (input.tagName === 'SELECT') {
      assert.ok([...input.options].some(option => option.value === value), 'Current select option must exist: ' + name);
      // linkedom's select.value is read-only. Change selection using the
      // option attributes its actual value getter reads, then emit change.
      for (const option of input.options) {
        if (option.value === value) option.setAttribute('selected', '');
        else option.removeAttribute('selected');
      }
    } else input.value = value;
    input.dispatchEvent(new window.Event(input.tagName === 'SELECT' || input.type === 'checkbox' ? 'change' : 'input'));
  };
  // Leaving a field: its change event.
  const leave = name => { const input = find('[name="' + name + '"]'); assert.ok(input, name); input.dispatchEvent(new window.Event('change')); };
  return { document: window.document, ctx, state, writes, dialogs, loads, find, change, leave, redraw,
    setSnapshot: value => { snapshot = value; redraw(); }, advance: ms => { clock += ms; redraw(); } };
}
async function testedHttp(f) {
  f.change('http.enabled', true);
  f.change('http.origin', 'http://broker.fixture:18490');
  f.change('http.listenPort', '18491');
  f.change('primaryOrigin', 'http://broker.fixture:18490');
  f.find('[data-access-test]').click(); await flush();
}

test('unobserved recovery never appears as a new or applied configuration', t => {
  const f = fixture(t, { snapshot: response({ applied: null, state: 'recovery_required', scope: 'unverified', currentObserved: false, applySupported: false, lastKnownConfig: config() }) });
  assert.match(f.find('.server-access-current').textContent, /현재 설정 확인 필요/);
  assert.doesNotMatch(f.find('.server-access-current').textContent, /https:\/\/broker.fixture/);
  assert.match(f.find('[data-access-draft-status]').textContent, /현재 설정을 읽지 못했어요/);
  assert.doesNotMatch(f.document.body.textContent, /새 설정 · 미적용|현재 적용된 설정이에요/);
  assert.equal(f.find('[data-access-test]').disabled, true);
  assert.equal(f.find('[data-access-save]').disabled, true);
  assert.equal(f.writes.length, 0);
});

test('observed recovery takes priority over a stale completed operation', t => {
  const f = fixture(t);
  f.state.ui.serverAccess.operation = { operationId: 'old-operation', state: 'applied' };
  f.setSnapshot(response({ revision: nextRev, state: 'recovery_required', scope: 'unverified', currentObserved: true, applySupported: false }));
  assert.match(f.find('.server-access-current').textContent, /현재 관측 설정/);
  assert.match(f.find('[data-access-draft-status]').textContent, /설정과 적용 기록이 달라요/);
  assert.doesNotMatch(f.find('[data-access-draft-status]').textContent, /적용된 것을 확인/);
  assert.equal(f.find('[data-access-save]').disabled, true);
});

test('recovery allows a read-only reload while configuration writes remain blocked', async t => {
  const f = fixture(t, { snapshot: response({ state: 'recovery_required', currentObserved: false, applied: null, applySupported: false }) });
  const reload = [...f.document.querySelectorAll('button')].find(node => node.textContent.includes('현재 설정 다시 불러오기'));
  assert.equal(reload.disabled, false);
  reload.click(); await flush();
  assert.deepEqual(f.loads, [{ key: 'serverAccess', options: { force: true } }]);
  assert.equal(f.writes.length, 0);
});

test('both transport toggles are independent; current settings never change on edit', t => {
  const f = fixture(t);
  assert.equal(f.find('[name="https.enabled"]').checked, true);
  assert.equal(!!f.find('[name="http.enabled"]').checked, false);
  f.change('http.enabled', true);
  assert.equal(f.find('[name="https.enabled"]').checked, true);
  f.change('https.enabled', false);
  assert.equal(f.find('[name="http.enabled"]').checked, true);
  assert.equal(f.writes.length, 0);
  assert.match(f.find('.server-access-current').textContent, /https:\/\/broker.fixture/);
  assert.match(f.document.body.textContent, /암호화되지 않아요/);
});

test('exact test/confirmed-save DTO, local NAT assistance and GET-proven applied state', async t => {
  const f = fixture(t); await testedHttp(f);
  const tested = f.writes[0];
  assert.equal(tested.path, '/server-access/test'); assert.equal(tested.body.baseRevision, rev);
  assert.deepEqual(Object.keys(tested.body).sort(), ['baseRevision', 'config']);
  assert.equal(tested.body.config.https.enabled, true); assert.equal(tested.body.config.http.enabled, true);
  assert.equal(tested.body.config.http.listenPort, 18491); assert.equal(tested.body.config.https.certFile, null);
  f.change('assist.targetIp', '192.0.2.20'); f.change('assist.proxyPort', '8443');
  assert.match(f.find('[data-access-forward-rows]').textContent, /TCP 443192\.0\.2\.20:8443/);
  assert.match(f.find('[data-access-forward-rows]').textContent, /TCP 18490192\.0\.2\.20:18491/);
  f.find('[data-access-save]').click(); assert.equal(f.dialogs.length, 1);
  assert.match(f.dialogs[0].options.description, /HTTP.*암호화하지/);
  assert.match(f.dialogs[0].options.description, /Host·앱 주소는 직접/);
  assert.match(f.dialogs[0].options.description, /계정·데스크톱 배정은 유지/);
  assert.doesNotMatch(f.dialogs[0].options.description, /연결이 없을 때/, 'a save no longer waits for people to leave');
  assert.equal(f.writes.length, 1, 'opening confirmation never saves');
  await f.dialogs[0].action(); await flush();
  assert.deepEqual(f.writes[1], { path: '/server-access/save', method: 'POST', body: {
    config: tested.body.config, testId: 'fixture-test', baseRevision: rev, confirm: true,
  } });
  assert.match(f.find('[data-access-draft-status]').textContent, /새 설정을 적용하고 있어요/);
  assert.equal(f.find('[data-access-cancel]'), null, 'nothing to cancel while it is simply applying');
  assert.equal(f.find('[data-access-save]').disabled, true);
  assert.equal(f.document.body.textContent.includes('새 설정이 적용된 것을 확인'), false);
  f.setSnapshot(response({ revision: nextRev, applied: tested.body.config, state: 'applied', operationId: 'fixture-operation' }));
  assert.match(f.find('[data-access-draft-status]').textContent, /적용된 것을 확인/);
  assert.match(f.find('.server-access-current').textContent, /http:\/\/broker.fixture:18490/);
});

test('every edit fences a late test reply', async t => {
  const waiting = gate();
  const f = fixture(t, { handler: async () => waiting.promise });
  f.change('https.origin', 'https://new.fixture'); f.find('[data-access-test]').click();
  f.change('https.origin', 'https://later.fixture');
  waiting.resolve({ testId: 'retired-test', revision: rev, ok: true, checks: [{ id: 'configuration', ok: true }], expiresAt: 1060000 });
  await flush(); assert.equal(f.find('[data-access-save]').disabled, true);
  assert.equal(f.state.ui.serverAccess.receipt, null);
});

test('expiry prevents a save without starting another test or operation', async t => {
  const f = fixture(t); await testedHttp(f);
  f.advance(60001); f.find('[data-access-save]').click();
  assert.equal(f.dialogs.length, 0); assert.equal(f.find('[data-access-save]').disabled, true);
  assert.equal(f.writes.length, 1);
});

test('changed revision keeps the draft, invalidates the test and requires explicit reset', async t => {
  const f = fixture(t); await testedHttp(f);
  const updated = config(); updated.https.origin = updated.primaryOrigin = 'https://other-admin.fixture';
  f.setSnapshot(response({ revision: nextRev, applied: updated }));
  assert.equal(f.find('[name="http.origin"]').value, 'http://broker.fixture:18490');
  assert.equal(f.find('[data-access-save]').disabled, true);
  assert.equal(f.find('[data-access-test]').disabled, true);
  assert.match(f.document.body.textContent, /다른 관리자가/);
  [...f.document.querySelectorAll('button')].find(node => node.textContent.includes('현재 설정 다시 불러오기')).click();
  await flush();
  // The unsaved draft is discarded only after the operator confirms.
  const discard = f.dialogs.at(-1); assert.equal(discard.title, '바꾼 내용을 버릴까요?'); assert.match(discard.options.description, /저장하지 않은 접속 설정이 사라져요/); assert.equal(discard.options.danger, true);
  assert.equal(f.find('[name="http.origin"]').value, 'http://broker.fixture:18490', 'nothing is discarded before the answer');
  await discard.action(); await flush();
  assert.equal(f.find('[name="https.origin"]').value, 'https://other-admin.fixture');
});

test('confirmation rechecks edits and current administrator before sending a save', async t => {
  const f = fixture(t); await testedHttp(f); f.find('[data-access-save]').click();
  f.change('http.listenPort', '19001');
  await assert.rejects(f.dialogs[0].action(), /다시 검사/);
  assert.equal(f.writes.length, 1);
  f.state.user = { id: 'different-admin' }; f.redraw();
  await assert.rejects(f.dialogs[0].action(), /다시 검사/);
  assert.equal(f.writes.length, 1);
});

test('direct TLS paths stay separate from contents and invalid proxy ranges are not tested', async t => {
  const f = fixture(t); f.change('https.mode', 'direct');
  assert.ok(f.find('[name="https.certFile"]')); assert.ok(f.find('[name="https.keyFile"]'));
  f.change('https.certFile', '/etc/multitap/server.crt'); f.change('https.keyFile', '/etc/multitap/server.key');
  f.change('trustedProxies', '192.0.2.0/24'); f.find('[data-access-test]').click(); await flush();
  assert.equal(f.writes.length, 0); assert.match(f.document.body.textContent, /프록시 IP/);
  f.change('trustedProxies', '192.0.2.5\n2001:db8::5'); f.find('[data-access-test]').click(); await flush();
  assert.equal(f.writes[0].body.config.https.certFile, '/etc/multitap/server.crt');
  assert.equal(f.writes[0].body.config.https.keyFile, '/etc/multitap/server.key');
  assert.deepEqual(f.writes[0].body.config.trustedProxies, ['192.0.2.5', '2001:db8::5']);
  f.change('https.mode', 'proxy'); f.find('[data-access-test]').click(); await flush();
  assert.equal(f.writes[1].body.config.https.certFile, null); assert.equal(f.writes[1].body.config.https.keyFile, null);
});

test('rollback and unsupported/unknown state do not claim new addresses are applied', async t => {
  const desired = config(); desired.https.origin = desired.primaryOrigin = 'https://not-applied.fixture';
  const f = fixture(t, { snapshot: response({ pending: { config: desired, state: 'failed_rolled_back', errorCode: 'listener_failed' } }) });
  assert.match(f.document.body.textContent, /이전 설정으로 돌아갔어요/);
  assert.equal(f.find('.server-access-current').textContent.includes('https://not-applied.fixture'), false);
  f.setSnapshot(response({ pending: { config: desired, state: 'future_operation_state' } }));
  assert.match(f.document.body.textContent, /작업 상태를 확인/);
  assert.equal(f.find('[data-access-test]').disabled, true);
  assert.equal(validResponse({ protocol: 'server-access-v1', revision: rev }), false);
});

test('no current response exposes no fabricated address or save control', t => {
  const f = fixture(t, { snapshot: null });
  assert.equal(f.find('[data-access-save]'), null);
  assert.equal(f.find('[name="https.origin"]'), null);
  assert.equal(f.document.body.textContent.includes('broker.fixture'), false);
  assert.equal(f.writes.length, 0);
});

test('modern preflight pending route is save-ready, not an external connection success', async t => {
  const f = fixture(t, { handler: async path => path.endsWith('/test') ? {
    testId: '1'.repeat(64), baseRevision: rev, configHash: '2'.repeat(64), scope: 'preflight',
    requiresRestart: true, expiresAt: '1970-01-01T00:17:40.000Z',
    checks: [{ name: 'configuration', status: 'passed' }, { name: 'external_route', status: 'pending', code: 'pending_until_apply' }],
  } : { accepted: true, operationId: 'matching-operation', state: 'waiting_idle' } });
  await testedHttp(f);
  assert.equal(f.writes[0].body.config.protocol, 'server-access-v1');
  assert.match(f.find('[data-access-test-status]').textContent, /사전 검사 통과/);
  assert.match(f.find('[data-access-test-status]').textContent, /실제 연결은 적용 후/);
  assert.match(f.find('.server-access-checks').textContent, /적용 후 확인/);
  assert.equal(f.find('[data-access-save]').disabled, false);
  f.find('[data-access-save]').click(); await f.dialogs[0].action();
  const desired = f.writes[0].body.config;
  f.setSnapshot({ protocol: 'server-access-v1', revision: rev, config: config(), state: 'health_check',
    pendingConfig: desired, operationId: 'matching-operation', applySupported: true });
  assert.match(f.find('[data-access-draft-status]').textContent, /새 주소의 연결을 확인/);
  assert.equal(f.find('.server-access-current').textContent.includes('http://broker.fixture:18490'), false);
  f.setSnapshot(response({ revision: nextRev, applied: desired, state: 'applied', operationId: 'other-operation' }));
  assert.equal(f.find('[data-access-draft-status]').textContent.includes('적용된 것을 확인'), false);
  f.setSnapshot(response({ revision: nextRev, applied: desired, state: 'applied', operationId: 'matching-operation' }));
  assert.match(f.find('[data-access-draft-status]').textContent, /적용된 것을 확인/);
});

test('bootstrap defaults recommend HTTPS without inventing addresses and unsupported apply stays read-only', t => {
  const f = fixture(t, { snapshot: { protocol: 'server-access-v1', revision: rev, config: null,
    state: 'idle', managed: false, applySupported: true, pendingConfig: null } });
  assert.equal(f.find('[name="https.enabled"]').checked, true);
  assert.equal(!!f.find('[name="http.enabled"]').checked, false);
  assert.equal(f.find('[name="https.origin"]').value, '');
  assert.equal(f.find('[name="https.listenHost"]').value, '');
  assert.equal(f.find('[name="https.listenPort"]').value, '');
  assert.match(f.find('.server-access-current').textContent, /아직 관리된 설정/);
  f.setSnapshot(response({ applySupported: false }));
  assert.equal(f.find('[data-access-test]').disabled, true);
  assert.equal(f.find('[data-access-save]').disabled, true);
  assert.match(f.document.body.textContent, /이 설치에서는 접속 설정을 바꿀 수 없어요/);
  assert.equal(f.writes.length, 0);
});

test('bootstrap uses only observed listener as unapplied draft and keeps disabled internal fields editable', async t => {
  const f = fixture(t, { snapshot: { protocol: 'server-access-v1', revision: 'bootstrap', applied: null,
    // The shape the server really sends (src/http.mjs serverInfo.listener: the socket's local address and port).
    state: 'idle', managed: true, supported: true, pending: null, bootstrapListener: { address: '127.0.0.1', port: 19400 } } });
  assert.match(f.find('.server-access-editor').textContent, /새 설정 · 미적용/);
  assert.equal(f.find('[name="https.listenHost"]').value, '127.0.0.1');
  assert.equal(f.find('[name="https.listenPort"]').value, '19400');
  assert.equal(f.find('[name="http.listenHost"]').value, '127.0.0.1');
  assert.equal(f.find('[name="http.listenPort"]').value, '19400');
  assert.notEqual(f.find('[name="http.listenPort"]').disabled, true);
  assert.equal(f.find('[name="https.origin"]').value, '');
  f.change('https.origin', 'https://new-install.fixture');
  f.change('primaryOrigin', 'https://new-install.fixture');
  f.find('[data-access-test]').click(); await flush();
  assert.equal(f.writes.length, 1);
  assert.equal(f.writes[0].body.baseRevision, 'bootstrap');
  assert.equal(f.writes[0].body.config.http.enabled, false);
  assert.equal(f.writes[0].body.config.http.listenPort, 19400);
  assert.equal(f.find('.server-access-current').textContent.includes('https://new-install.fixture'), false);
});

test('NAT help uses actual Docker host port and labels preflight target as unapplied', async t => {
  const applied = config(); applied.http.enabled = true; applied.http.origin = 'http://broker.fixture:18490';
  const f = fixture(t, { snapshot: response({ applied, dockerPorts: [{ scheme: 'http', protocol: 'tcp', hostPort: 28480, containerPort: 18480 }], plannedDockerPorts: [] }),
    handler: async () => ({ testId: '1'.repeat(64), baseRevision: rev, configHash: '2'.repeat(64), scope: 'preflight', requiresRestart: true,
      expiresAt: '1970-01-01T00:17:40.000Z', checks: [{ name: 'configuration', status: 'passed' }],
      plannedDockerPorts: [{ scheme: 'http', protocol: 'tcp', hostPort: 38481, containerPort: 18481 }] }) });
  f.change('assist.targetIp', '192.0.2.20');
  assert.match(f.find('[data-access-forward-rows]').textContent, /192\.0\.2\.20:28480Docker 게시 · 컨테이너 18480/);
  f.change('http.listenPort', '18481'); f.find('[data-access-test]').click(); await flush();
  assert.match(f.find('[data-access-forward-rows]').textContent, /192\.0\.2\.20:38481Docker 예정·미적용 · 컨테이너 18481/);
  assert.equal(f.find('.server-access-current').textContent.includes('38481'), false);
  assert.equal(f.writes[0].body.config.http.listenPort, 18481);
  assert.equal(Object.hasOwn(f.writes[0].body.config, 'dockerPorts'), false);
});

// A Docker Compose install without an updater applies a save itself
// (src/self-access.mjs): what its status carries beside the usual fields.
const dockerFixed = (extra = {}) => ({ listenHost: '0.0.0.0', listenPort: 18480, httpsMode: 'proxy', certFile: null, keyFile: null, idlePorts: { https: 18443, http: 18481 },
  publishedPort: 18480, publishedHost: '0.0.0.0', serverAddress: '192.0.2.10', ...extra });
const dockerPolicy = (http = {}, https = {}) => ({ protocol: 'server-access-v1', primaryOrigin: 'http://192.0.2.10:18480',
  https: { enabled: false, origin: null, mode: 'proxy', listenHost: '0.0.0.0', listenPort: 18443, certFile: null, keyFile: null, ...https },
  http: { enabled: true, origin: 'http://192.0.2.10:18480', listenHost: '0.0.0.0', listenPort: 18480, ...http }, trustedProxies: ['127.0.0.1', '::1'] });
const docker = (extra = {}) => ({ protocol: 'server-access-v1', revision: rev, applied: dockerPolicy(), pending: null, state: 'idle', managed: true, supported: true, deploymentKind: 'docker', selfManaged: true,
  managementOrigin: 'http://192.0.2.10:18480', fixed: dockerFixed(), publicIp: { offered: true, follow: false, origin: null }, udpPort: 18481,
  dockerPorts: ['https', 'http'].map(scheme => ({ hostPort: 18480, containerPort: 18480, protocol: 'tcp', scheme })), ...extra });

test('A Docker Compose install edits its addresses on the page; the first setup suggests its own address and saves it as it is', async t => {
  const f = fixture(t, { snapshot: docker({ applied: null }) });
  // The same editor as an install with an updater, with test and save; no command to copy.
  assert.ok(f.find('[data-access-test]')); assert.ok(f.find('[data-access-save]'));
  assert.equal(f.find('[data-access-env]'), null); assert.equal(f.find('[data-access-command]'), null);
  assert.equal(f.document.body.textContent.includes('multitap address'), false);
  // What the container fixes is not a field.
  for (const name of ['http.listenHost', 'http.listenPort', 'https.listenHost', 'https.listenPort', 'https.mode', 'https.certFile']) assert.equal(f.find('[name="' + name + '"]'), null, name);
  assert.equal(f.find('[name="http.enabled"]').checked, true); assert.ok(!f.find('[name="https.enabled"]').checked);
  assert.equal(f.find('[name="http.origin"]').value, 'http://192.0.2.10:18480');
  assert.equal(f.find('[data-access-fixed="http"]').textContent, 'Docker가 여는 포트 · TCP 18480\n서버 IP로 만든 주소에는 이 포트를 쓰세요.');
  assert.equal(f.find('[data-access-fixed="https"]'), null);
  f.find('[data-access-test]').click(); await flush();
  assert.deepEqual(f.writes[0].body.config, { protocol: 'server-access-v1', primaryOrigin: 'http://192.0.2.10:18480',
    https: { enabled: false, origin: null, listenHost: '0.0.0.0', listenPort: 18443, mode: 'proxy', certFile: null, keyFile: null },
    http: { enabled: true, origin: 'http://192.0.2.10:18480', listenHost: '0.0.0.0', listenPort: 18480 }, trustedProxies: [] });
  // The suggestion was not typed, and it can still be saved.
  assert.equal(f.find('[data-access-save]').disabled, false);
  f.find('[data-access-save]').click(); await f.dialogs[0].action(); await flush();
  assert.deepEqual(f.writes[1], { path: '/server-access/save', method: 'POST', body: { config: f.writes[0].body.config, testId: 'fixture-test', baseRevision: rev, confirm: true } });
  f.setSnapshot(docker({ revision: nextRev, applied: dockerPolicy({}, {}), state: 'applied', operationId: 'fixture-operation' }));
  assert.match(f.find('[data-access-draft-status]').textContent, /적용된 것을 확인/);
  assert.equal(f.find('[data-access-save]').disabled, true);
});

test('A Docker Compose install keeps the container\'s listener in every change; its own TLS offers no HTTP and a loopback-only port says so', async t => {
  const f = fixture(t, { snapshot: docker() });
  assert.equal(f.find('[data-access-save]').disabled, true, 'the applied policy is no edit');
  assert.equal(f.find('[data-access-draft-status]').textContent, '');
  f.change('https.enabled', true); f.change('https.origin', 'https://vdi.example.com');
  assert.equal(f.find('[data-access-fixed="https"]').textContent, '프록시가 보낼 곳 · 서버의 TCP 18480 포트\n같은 서버의 프록시라면 아래 ‘신뢰할 프록시 IP’에 Docker 게이트웨이 IP를 넣으세요.');
  // An address with another external port (a router maps it) leaves the listener alone.
  f.find('[data-access-extra-add="http"]').click(); f.change('http.extra.0', 'http://203.0.113.7:8080');
  f.find('[data-access-test]').click(); await flush();
  assert.deepEqual(f.writes[0].body.config.https, { enabled: true, origin: 'https://vdi.example.com', listenHost: '0.0.0.0', listenPort: 18480, mode: 'proxy', certFile: null, keyFile: null });
  assert.deepEqual(f.writes[0].body.config.http, { enabled: true, origin: 'http://192.0.2.10:18480', extraOrigins: ['http://203.0.113.7:8080'], listenHost: '0.0.0.0', listenPort: 18480 });
  // The real Docker port in the port-forwarding help.
  f.change('assist.targetIp', '192.0.2.10');
  assert.match(f.find('[data-access-forward-rows]').textContent, /TCP 8080192\.0\.2\.10:18480/);

  // compose.direct-https.yaml: the broker's own TLS, with the certificate the container mounts.
  const direct = dockerPolicy({ enabled: false, origin: null, listenPort: 18481 }, { enabled: true, origin: 'https://vdi.example.com', mode: 'direct', listenPort: 18480, certFile: '/etc/multitap/tls/cert.pem', keyFile: '/etc/multitap/tls/key.pem' });
  direct.primaryOrigin = 'https://vdi.example.com';
  f.state.ui = {}; f.setSnapshot(docker({ applied: direct, managementOrigin: 'https://vdi.example.com', fixed: dockerFixed({ httpsMode: 'direct', certFile: '/etc/multitap/tls/cert.pem', keyFile: '/etc/multitap/tls/key.pem' }), publicIp: { offered: false, follow: false, origin: null } }));
  assert.equal(f.find('[name="http.enabled"]').disabled, true); assert.equal(f.find('[name="https.enabled"]').disabled, true);
  assert.equal(f.find('[data-access-fixed="http"]').textContent, '서버가 직접 HTTPS를 받는 설치에서는 HTTP 주소를 함께 쓸 수 없어요.');
  assert.match(f.find('[data-access-fixed="https"]').textContent, /서버가 직접 HTTPS를 받아요/);
  f.change('https.origin', 'https://desk.example.com'); f.change('primaryOrigin', 'https://desk.example.com');
  f.find('[data-access-test]').click(); await flush();
  assert.deepEqual(f.writes[1].body.config.https, { enabled: true, origin: 'https://desk.example.com', listenHost: '0.0.0.0', listenPort: 18480, mode: 'direct', certFile: '/etc/multitap/tls/cert.pem', keyFile: '/etc/multitap/tls/key.pem' });

  // Published on loopback only (an HTTPS proxy on the same machine): HTTP is for the server's shell, and nothing follows the public IP.
  const secure = dockerPolicy({ enabled: false, origin: null, listenPort: 18481 }, { enabled: true, origin: 'https://vdi.example.com', listenPort: 18480 });
  secure.primaryOrigin = 'https://vdi.example.com';
  f.state.ui = {}; f.setSnapshot(docker({ applied: secure, managementOrigin: 'https://vdi.example.com', fixed: dockerFixed({ publishedHost: '127.0.0.1' }), publicIp: { offered: false, follow: false, origin: null } }));
  f.change('http.enabled', true);
  assert.equal(f.find('[data-access-fixed="http"]').textContent, 'Docker 포트가 서버 안에서만 열려 있어요.\nHTTP 주소는 서버에서 multitap address로 정하세요.');
  assert.equal(f.find('[data-access-follow]'), null);
  assert.equal(f.find('[name="http.followPublicIp"]'), null);
});

test('the answers only a Docker Compose install gives say what to do', async t => {
  for (const [code, text] of [
    ['server_access_published_port_conflict', /Docker가 여는 포트를 써야 해요/], ['server_access_published_loopback_conflict', /서버 안에서만 열려 있어요/],
    ['server_access_direct_tls_http_conflict', /HTTP 주소를 함께 쓸 수 없어요/], ['server_access_public_ip_conflict', /공인 IP 따라가기를 켤 수 없어요/]]) {
    const f = fixture(t, { snapshot: docker(), handler: () => { const error = Error(code); error.code = code; throw error; } });
    f.find('[data-access-extra-add="http"]').click(); f.change('http.extra.0', 'http://192.0.2.10:8080');
    f.find('[data-access-test]').click(); await flush();
    assert.match(f.find('.server-access-error').textContent, text, code);
  }
});

test('HTTP listener follows its external address, shares the HTTPS proxy listener and keeps a typed value', t => {
  const f = fixture(t);
  assert.equal(f.find('[data-access-waiting]'), null);
  f.change('http.enabled', true);
  const advanced = f.find('[data-keep-open="access-http"]');
  assert.equal(advanced.hasAttribute('open'), false, 'internal fields stay collapsed');
  assert.match(advanced.textContent, /공유기나 Docker가 포트를 바꿔 전달할 때만 바꾸세요/);
  f.change('http.origin', 'http://broker.fixture:18400');
  assert.equal(f.find('[name="http.listenPort"]').value, '18400');
  assert.equal(f.find('[name="http.listenHost"]').value, '127.0.0.1', 'same port as the HTTPS proxy: one shared listener');
  f.change('http.origin', 'http://192.0.2.10:18490');
  assert.equal(f.find('[name="http.listenPort"]').value, '18490');
  assert.equal(f.find('[name="http.listenHost"]').value, '0.0.0.0');
  f.change('http.origin', 'http://192.0.2.10');
  assert.equal(f.state.ui.serverAccess.draft.http.listenPort, '80');
  f.change('http.listenPort', '18491');
  f.change('http.origin', 'http://192.0.2.10:18492');
  assert.equal(f.find('[name="http.listenPort"]').value, '18491');
  assert.equal(f.writes.length, 0);
});

test('a port mismatch refused by the server test names both ports and keeps saving blocked', async t => {
  const f = fixture(t, { handler: async path => {
    if (path.endsWith('/test')) { const error = Error('요청을 처리하지 못했어요.'); error.code = 'server_access_external_port_conflict'; throw error; }
    throw Error('unexpected ' + path);
  } });
  await testedHttp(f);
  assert.match(f.find('.server-access-error').textContent, /외부 주소의 포트\(18490\)와 내부 수신 포트\(18491\)가 달라요\. 같게 맞추세요\./);
  assert.equal(f.find('[data-access-save]').disabled, true);
});

// Saving ends the existing connections and applies; the
// server does not wait for people to leave. Only its own unfinished work can
// hold a save, and then the page says what, until when, and offers 취소.
const waitingSnapshot = (extra = {}) => response({ state: 'waiting_idle', operationId: 'fixture-operation', pending: { config: config(), state: 'waiting_idle', operationId: 'fixture-operation' }, ...extra });
test('a save that is ending connections says so at the top of the form and offers no cancel', t => {
  const f = fixture(t, { snapshot: waitingSnapshot() });
  const editor = f.find('.server-access-editor'), text = editor.textContent;
  assert.equal(f.find('[data-access-waiting]').textContent, '새 설정을 적용하고 있어요. 보통 몇 초면 끝나요.');
  assert.ok(text.indexOf('새 설정을 적용하고 있어요. 보통') < text.indexOf('HTTPS 접속'), 'shown above the endpoints');
  assert.equal(f.find('[data-access-cancel]'), null);
  assert.equal(f.find('[name="http.enabled"]').disabled, true);
  assert.equal(f.find('[data-access-test]').disabled, true);
  assert.doesNotMatch(f.document.body.textContent, /연결이 끝나면/, 'the page never says it waits for connections to end');
});

test('the one confirmation says how many people are connected and that their connections end', async t => {
  const description = async snapshot => { const f = fixture(t, { snapshot }); await testedHttp(f); f.find('[data-access-save]').click(); return f.dialogs[0].options.description; };
  assert.equal(await description(response({ connections: { people: 3, sessions: 4, consoles: 0 } })),
    '접속 중인 3명의 연결이 끊겨요. 서버가 몇 초 동안 다시 시작될 수 있어요.\nHTTP는 로그인 정보와 화면을 암호화하지 않아요.\nHost·앱 주소는 직접 바꿔 주세요. 계정·데스크톱 배정은 유지돼요.');
  const nobody = await description(response({ connections: { people: 0, sessions: 0, consoles: 0 } }));
  // Nobody connected: nothing ends, and the restart the save may cause is still said before the click.
  assert.match(nobody, /^저장하면 바로 적용해요\. 서버가 몇 초 동안 다시 시작될 수 있어요\.\n/); assert.doesNotMatch(nobody, /끊겨요/);
  assert.match(await description(response()), /^접속 중인 연결은 끊겨요\. 서버가 몇 초 동안 다시 시작될 수 있어요\.\n/, 'a server that sends no count still says connections end');
  for (const text of [nobody, await description(response({ connections: { people: 12, sessions: 12, consoles: 1 } }))]) assert.equal(text.split('\n').length, 3, 'two or three short lines');
  // HTTPS only: the middle line is about the console itself.
  const f = fixture(t, { snapshot: response({ connections: { people: 1, sessions: 1, consoles: 0 } }) });
  f.change('https.origin', 'https://new.fixture'); f.find('[data-access-test]').click(); await flush(); f.find('[data-access-save]').click();
  assert.equal(f.dialogs[0].options.description, '접속 중인 1명의 연결이 끊겨요. 서버가 몇 초 동안 다시 시작될 수 있어요.\n관리자 페이지 연결이 잠시 끊길 수 있어요.\nHost·앱 주소는 직접 바꿔 주세요. 계정·데스크톱 배정은 유지돼요.');
});

test('a save held by the server\'s own work shows the reason in plain words, a bounded wait and a cancel that withdraws it', async t => {
  const waitUntil = new Date(2026, 10, 10, 14, 32, 5).toISOString();
  const blockers = [{ code: 'server_access_host_updates_busy', count: 3 }];
  let refuse = null;
  const f = fixture(t, { snapshot: waitingSnapshot({ blockers, waitUntil, idleReasons: ['server_access_host_updates_busy'] }), handler: async path => {
    if (!path.endsWith('/cancel')) throw Error('unexpected ' + path);
    if (refuse) { const error = Error('요청을 처리하지 못했어요.'); error.code = refuse; throw error; }
    return { accepted: true, operationId: 'fixture-operation', state: 'failed', cancelled: true };
  } });
  assert.equal(f.find('[data-access-waiting]').textContent, '데스크톱 3대의 Host 업데이트가 끝나기를 기다려요.\n14:32까지 기다리고, 그때도 안 끝나면 적용하지 않아요.');
  assert.equal(f.find('[data-access-draft-status]').textContent, '진행 중인 작업이 끝나면 적용해요. 현재 설정은 유지돼요.');
  assert.doesNotMatch(f.document.body.textContent, /server_access_|세션/, 'no internal word reaches the page');
  const cancel = f.find('[data-access-cancel]'); assert.equal(cancel.textContent, '취소'); assert.equal(cancel.disabled, false);
  assert.equal(f.find('[name="http.enabled"]').disabled, true, 'the form stays locked while it waits');
  // Every reason the engine can report has its own line.
  f.setSnapshot(waitingSnapshot({ waitUntil, blockers: [...blockers, { code: 'server_access_desktop_settings_busy', count: 1 }, { code: 'server_access_power_busy', count: 2 }] }));
  assert.equal(f.find('[data-access-waiting]').textContent, '데스크톱 3대의 Host 업데이트가 끝나기를 기다려요.\n데스크톱 1대의 설정 변경이 끝나기를 기다려요.\n데스크톱 2대의 전원 작업이 끝나기를 기다려요.\n14:32까지 기다리고, 그때도 안 끝나면 적용하지 않아요.');
  f.setSnapshot(waitingSnapshot({ blockers: [{ code: 'server_access_activity_unverified', count: 0 }] }));
  assert.equal(f.find('[data-access-waiting]').textContent, '서버 상태를 확인하고 있어요.\n곧 끝나지 않으면 적용하지 않아요.');
  f.setSnapshot(waitingSnapshot({ blockers, waitUntil }));

  // Too late: the server already started applying. The page says so and keeps waiting.
  refuse = 'server_access_cancel_too_late'; f.find('[data-access-cancel]').click(); await flush();
  assert.deepEqual(f.writes.at(-1), { path: '/server-access/cancel', method: 'POST', body: { operationId: 'fixture-operation' } });
  assert.equal(f.find('.server-access-error').textContent, '이미 적용을 시작해서 취소할 수 없어요.\n결과를 기다려 주세요.');
  assert.equal(f.find('[name="http.enabled"]').disabled, true);

  refuse = null; const loadsBefore = f.loads.length; f.find('[data-access-cancel]').click(); await flush();
  assert.equal(f.writes.filter(write => write.path === '/server-access/cancel').length, 2);
  assert.deepEqual(f.loads.slice(loadsBefore), [{ key: 'serverAccess', options: { force: true } }], 'the page reads the result back');
  // The server confirmed nothing changed: the form opens again with the applied settings.
  f.setSnapshot(response({ state: 'failed', operationId: 'fixture-operation', errorCode: 'server_access_cancelled', unchangedVerified: true }));
  assert.equal(f.find('[data-access-draft-status]').textContent, '저장을 취소했어요. 현재 설정은 그대로예요.');
  assert.equal(f.find('[data-access-waiting]'), null); assert.equal(f.find('[data-access-cancel]'), null);
  assert.notEqual(f.find('[name="http.enabled"]').disabled, true); assert.equal(f.find('[data-access-test]').disabled, false);
  assert.match(f.find('.server-access-current').textContent, /https:\/\/broker.fixture/);
});

test('a wait that ran out says what blocked it, keeps the current settings and opens the form again', async t => {
  const f = fixture(t); await testedHttp(f); f.find('[data-access-save]').click(); await f.dialogs[0].action(); await flush();
  assert.equal(f.find('[name="http.origin"]').disabled, true);
  f.setSnapshot(response({ state: 'failed', operationId: 'fixture-operation', errorCode: 'server_access_idle_timeout', unchangedVerified: true,
    blockers: [{ code: 'server_access_host_updates_busy', count: 3 }], idleReasons: ['server_access_host_updates_busy'] }));
  assert.equal(f.find('[data-access-draft-status]').textContent, '데스크톱 3대의 Host 업데이트가 끝나지 않아 적용하지 못했어요.\n현재 설정은 그대로예요. 끝난 뒤 다시 저장하세요.');
  assert.equal(f.find('[data-access-waiting]'), null);
  assert.notEqual(f.find('[name="http.origin"]').disabled, true, 'the form no longer stays locked after a timeout');
  assert.equal(f.find('[name="http.origin"]').value, 'http://broker.fixture:18490', 'the unsaved change is kept for another try');
  assert.equal(f.find('[data-access-save]').disabled, true, 'it has to be tested again first');
  assert.doesNotMatch(f.document.body.textContent, /server_access_/);
  // A failure the server could not prove harmless still asks to check the server.
  f.setSnapshot(response({ state: 'failed', operationId: 'fixture-operation', errorCode: 'server_access_fence_lost', unchangedVerified: false, pending: { config: config(), state: 'failed', operationId: 'fixture-operation' } }));
  assert.match(f.find('[data-access-draft-status]').textContent, /적용 결과나 복원을 확인하지 못했어요/);
  assert.equal(f.find('[data-access-test]').disabled, true);
});

// Bare addresses, several addresses per card, built-in help.
// One entry per way in; each step is its own line.
const steps = f => [...f.find('[data-access-outside-steps]').querySelectorAll('li')].map(way => [way.querySelector('strong').textContent, ...[...way.querySelectorAll('.outside-step p')].map(step => step.textContent)]);
test('an address without a scheme takes its card scheme, shows it on leaving the field and the HTTP listener follows it', async t => {
  const f = fixture(t);
  f.change('http.enabled', true);
  f.change('http.origin', '192.0.2.10:18490');
  assert.equal(f.find('[name="http.listenPort"]').value, '18490');
  assert.equal(f.find('[name="http.listenHost"]').value, '0.0.0.0');
  assert.equal(f.find('[data-access-external="http"]').textContent, '외부 TCP 포트 · 18490');
  f.leave('http.origin');
  assert.equal(f.find('[name="http.origin"]').value, 'http://192.0.2.10:18490');
  f.change('https.origin', ' vdi.example.com');
  f.find('[data-access-test]').click(); await flush();
  const sent = f.writes[0].body.config;
  assert.equal(sent.https.origin, 'https://vdi.example.com'); assert.equal(sent.primaryOrigin, 'https://vdi.example.com', 'the chosen main address follows its edit');
  assert.equal(sent.http.origin, 'http://192.0.2.10:18490'); assert.equal(sent.http.listenPort, 18490);
  assert.equal(Object.hasOwn(sent.http, 'extraOrigins'), false, 'no extra list without extra addresses');
  assert.equal(f.find('[name="https.origin"]').getAttribute('inputmode'), 'url');
});

test('a card takes extra addresses with their own port; the chosen main address goes first in the exact test/save DTO', async t => {
  const f = fixture(t);
  f.change('http.enabled', true); f.change('http.origin', 'http://192.0.2.10:18480');
  const card = f.find('[data-access-endpoint="http"]').textContent;
  assert.match(card, /앱·Host·브라우저에 넣는 주소예요\.\n여기 적은 주소로만 접속돼요\./);
  assert.match(card, /공유기 포트포워딩으로 밖에서 들어오는 주소\(공인 IP 등\)도 추가하세요\./);
  assert.match(f.find('[data-access-endpoint="https"]').textContent, /다른 도메인으로도 열면 그 주소도 추가하세요\./);
  f.find('[data-access-extra-add="http"]').click();
  f.change('http.extra.0', '203.0.113.7:8080');
  assert.equal(f.find('[data-access-external="http.0"]').textContent, '외부 TCP 포트 · 8080');
  assert.equal(f.find('[name="http.listenPort"]').value, '18480', 'the listener follows the first address only');
  f.change('http.extra.0', '203.0.113.7:18480'); f.leave('http.extra.0');
  assert.equal(f.find('[name="http.extra.0"]').value, 'http://203.0.113.7:18480');
  assert.deepEqual([...f.find('[name="primaryOrigin"]').options].map(option => option.value), ['', 'https://broker.fixture', 'http://192.0.2.10:18480', 'http://203.0.113.7:18480']);
  f.find('[data-access-test]').click(); await flush();
  assert.deepEqual(f.writes[0].body.config.http, { enabled: true, origin: 'http://192.0.2.10:18480', extraOrigins: ['http://203.0.113.7:18480'], listenHost: '0.0.0.0', listenPort: 18480 });
  assert.equal(f.writes[0].body.config.https.extraOrigins, undefined);
  f.change('primaryOrigin', 'http://203.0.113.7:18480');
  f.find('[data-access-test]').click(); await flush();
  const tested = f.writes[1].body.config;
  assert.equal(tested.primaryOrigin, 'http://203.0.113.7:18480');
  assert.deepEqual(tested.http, { enabled: true, origin: 'http://203.0.113.7:18480', extraOrigins: ['http://192.0.2.10:18480'], listenHost: '0.0.0.0', listenPort: 18480 });
  assert.equal(f.find('[name="http.origin"]').value, 'http://192.0.2.10:18480', 'the rows stay as typed');
  f.find('[data-access-save]').click(); await f.dialogs[0].action(); await flush();
  assert.deepEqual(f.writes[2], { path: '/server-access/save', method: 'POST', body: { config: tested, testId: 'fixture-test', baseRevision: rev, confirm: true } });
});

test('extra address rows are removed one by one and stop at four addresses per card', t => {
  const f = fixture(t);
  f.change('http.enabled', true); f.change('http.origin', 'http://192.0.2.10:18480');
  for (const index of [0, 1, 2]) { f.find('[data-access-extra-add="http"]').click(); f.change('http.extra.' + index, 'http://203.0.113.' + (index + 1) + ':18480'); }
  assert.equal(f.find('[data-access-extra-add="http"]').disabled, true);
  assert.equal(f.find('[data-access-extra-add="http"]').title, '주소는 4개까지 넣을 수 있어요.');
  f.find('[data-access-extra-remove="http.0"]').click();
  assert.deepEqual([...f.document.querySelectorAll('[data-access-extra] input')].map(input => input.value), ['http://203.0.113.2:18480', 'http://203.0.113.3:18480']);
  assert.equal(f.find('[data-access-extra-add="http"]').disabled, false);
  assert.equal(f.writes.length, 0);
});

test('an applied policy with extra addresses loads as rows and is not an edit', t => {
  const applied = config(); applied.http = { enabled: true, origin: 'http://192.0.2.10:18480', extraOrigins: ['http://203.0.113.7:18480'], listenHost: '0.0.0.0', listenPort: 18480 };
  const f = fixture(t, { snapshot: response({ applied }) });
  assert.equal(f.find('[name="http.extra.0"]').value, 'http://203.0.113.7:18480');
  assert.match(f.find('.server-access-current').textContent, /http:\/\/192\.0\.2\.10:18480\nhttp:\/\/203\.0\.113\.7:18480/);
  assert.equal(f.state.ui.serverAccess.dirty, false);
  assert.equal(validResponse(response({ applied: { ...applied, http: { ...applied.http, extraOrigins: 'http://203.0.113.7:18480' } } })), false);
});

test('집 밖에서 접속하려면: HTTPS first, then HTTP with the real TCP and UDP media ports, following the HTTP address', t => {
  const applied = config(); applied.http = { enabled: true, origin: 'http://192.0.2.10:18480', listenHost: '0.0.0.0', listenPort: 18480 };
  const f = fixture(t, { snapshot: response({ applied, udpPort: 18481 }) });
  const help = f.find('[data-access-outside-help]');
  assert.equal(help.tagName, 'DETAILS'); assert.equal(help.hasAttribute('open'), false, 'details stay folded');
  assert.equal(help.querySelector('summary').textContent, '집 밖에서 접속하려면');
  assert.deepEqual(steps(f), [
    ['도메인 + HTTPS · 안전해요 (권장)', '1도메인을 이 서버로 연결해요(리버스 프록시 등).', '2위 HTTPS 주소 칸에 그 도메인을 넣어요. https://vdi.example.com'],
    ['공인 IP + HTTP · 암호화는 안 돼요 (간단)', '1공유기의 포트 포워딩에서 TCP 18480 포트를 이 서버로 전달해요. 안 하면 밖에서는 들어오지 않아요.', '2‘추가 주소’에 공인 IP 주소를 넣어요. http://공인IP:18480', '3공인 IP는 통신사가 바꿀 수 있어요. 공유기의 DDNS 주소를 쓰면 바뀌어도 그대로예요. http://이름.iptime.org:18480'],
    ['권장 설정 화면 전송을 빠르게 · HTTPS·HTTP 모두', '공유기에서 UDP 18481도 이 서버로 전달해요. 프록시는 보통 TCP만 넘기니 HTTPS도 같아요.', '안 하면 화면이 TCP로 가서 조금 느릴 뿐, 접속은 돼요.'],
  ].map(way => way.map(text => text.replace(/^\d/, ''))));
  assert.match(help.textContent, /HTTP는 비밀번호와 화면이 암호화되지 않아요\./);
  assert.equal(help.querySelector('.pill'), null, '권장 / 간단 are plain words after the title');
  f.change('http.origin', 'http://192.0.2.10:18490');
  assert.equal(steps(f)[1][1], '공유기의 포트 포워딩에서 TCP 18490 포트를 이 서버로 전달해요. 안 하면 밖에서는 들어오지 않아요.');
});

test('without HTTP or a UDP media port the help invents no port', t => {
  const f = fixture(t);
  assert.deepEqual(steps(f)[1], ['공인 IP + HTTP · 암호화는 안 돼요 (간단)', '공유기의 포트 포워딩에서 HTTP 포트(TCP)를 이 서버로 전달해요. 안 하면 밖에서는 들어오지 않아요.', '‘추가 주소’에 공인 IP 주소를 넣어요. http://공인IP:포트', '공인 IP는 통신사가 바꿀 수 있어요. 공유기의 DDNS 주소를 쓰면 바뀌어도 그대로예요. http://이름.iptime.org:포트']);
  assert.doesNotMatch(f.find('[data-access-outside-steps]').textContent, /UDP/);
});

test('a refused address and a list without this console address say so plainly', async t => {
  const f = fixture(t, { handler: async () => { const error = Error('요청을 처리하지 못했어요.'); error.code = 'origin_not_allowed'; throw error; } });
  const outside = () => f.find('[data-access-console-outside]');
  assert.equal(outside().hidden, true, 'nothing edited yet');
  f.change('https.origin', 'https://broker2.fixture');
  assert.equal(outside().hidden, false);
  assert.equal(outside().textContent, '지금 연 주소(https://console.fixture)가 목록에 없어요.\n저장하면 이 주소로는 열리지 않아요.');
  f.find('[data-access-extra-add="https"]').click(); f.change('https.extra.0', 'console.fixture');
  assert.equal(outside().hidden, true);
  f.find('[data-access-test]').click(); await flush();
  assert.equal(f.find('.server-access-error').textContent, '지금 연 주소는 접속 설정에 없어서 막혔어요.\n저장된 주소로 열거나, 이 주소를 추가하세요.');
  assert.equal(f.find('[data-access-save]').disabled, true);
});

test('a rollback because the new address was unreachable says to check it and the port forwarding', t => {
  const desired = config(); desired.http = { enabled: true, origin: 'http://203.0.113.7:18480', listenHost: '0.0.0.0', listenPort: 18480 };
  const f = fixture(t, { snapshot: response({ pending: { config: desired, state: 'failed_rolled_back', errorCode: 'server_access_health_failed' } }) });
  assert.equal(f.find('[data-access-draft-status]').textContent, '새 주소로 서버에 닿지 못해 이전 설정으로 돌아갔어요.\n주소와 공유기 포트포워딩을 확인하세요.');
});

test('following the public IP is a switch in the HTTP card of a Docker Compose install; the address it registered has its own line', async t => {
  const followed = 'http://203.0.113.7:18480';
  const f = fixture(t, { snapshot: docker({ applied: dockerPolicy({ extraOrigins: [followed], followPublicIp: true }), publicIp: { offered: true, follow: true, origin: followed } }) });
  assert.equal(f.find('[name="http.followPublicIp"]').checked, true);
  assert.equal(f.find('[data-access-followed]').textContent, '지금 등록된 주소 · ' + followed);
  assert.equal(f.find('[name="http.extra.0"]'), null, 'the registered address is no editable row');
  assert.equal(f.find('[data-access-follow-router]').textContent, '공유기 포트 포워딩 · TCP 18480, UDP 18481 → 이 서버\n포워딩하지 않으면 밖에서는 들어오지 않아요.');
  assert.match(f.find('[data-access-endpoint="http"]').textContent, /로그인 정보와 화면이 암호화되지 않아요/);
  assert.equal([...f.find('[name="primaryOrigin"]').options].some(option => option.value === followed), false, 'never the primary address');
  assert.equal(f.find('[data-access-save]').disabled, true, 'the applied policy is no edit');
  // It takes one of the extra places: two more rows, then the button stops.
  f.find('[data-access-extra-add="http"]').click(); f.find('[data-access-extra-add="http"]').click();
  assert.equal(f.find('[data-access-extra-add="http"]').disabled, true);
  f.find('[data-access-extra-remove="http.1"]').click(); f.change('http.extra.0', 'http://vdi.example.com:18480');
  f.find('[data-access-test]').click(); await flush();
  assert.deepEqual(f.writes[0].body.config.http, { enabled: true, origin: 'http://192.0.2.10:18480', extraOrigins: ['http://vdi.example.com:18480', followed], followPublicIp: true, listenHost: '0.0.0.0', listenPort: 18480 });
  // Turned off, the address stays as an ordinary one the administrator can delete.
  f.change('http.followPublicIp', false);
  assert.equal(f.find('[data-access-followed]'), null); assert.equal(f.find('[data-access-follow-router]'), null);
  assert.equal(f.find('[name="http.extra.1"]').value, followed);
  f.find('[data-access-test]').click(); await flush();
  assert.deepEqual(f.writes[1].body.config.http, { enabled: true, origin: 'http://192.0.2.10:18480', extraOrigins: ['http://vdi.example.com:18480', followed], listenHost: '0.0.0.0', listenPort: 18480 });

  // Turned on where nothing is registered yet: only the switch travels; the server looks the address up.
  f.state.ui = {}; f.setSnapshot(docker({ udpPort: null }));
  assert.ok(!f.find('[name="http.followPublicIp"]').checked);
  assert.equal(f.find('[data-access-follow-router]'), null);
  f.change('http.followPublicIp', true);
  assert.equal(f.find('[data-access-followed]').textContent, '아직 등록된 주소가 없어요. 저장하면 바로 확인해요.');
  assert.equal(f.find('[data-access-follow-router]').textContent, '공유기 포트 포워딩 · TCP 18480 → 이 서버\n포워딩하지 않으면 밖에서는 들어오지 않아요.');
  f.find('[data-access-test]').click(); await flush();
  assert.deepEqual(f.writes[2].body.config.http, { enabled: true, origin: 'http://192.0.2.10:18480', followPublicIp: true, listenHost: '0.0.0.0', listenPort: 18480 });
  assert.equal(f.find('[data-access-save]').disabled, false);
  // No room left for the address: said where the switch is.
  f.state.ui = {}; f.setSnapshot(docker({ applied: dockerPolicy({ followPublicIp: true }), publicIp: { offered: true, follow: true, origin: null, errorCode: 'server_access_http_addresses_full' } }));
  assert.match(f.find('[data-access-follow-full]').textContent, /HTTP 주소가 가득 차서 공인 IP를 등록하지 못했어요/);
  assert.equal(f.find('[data-access-follow-conflict]'), null);
  f.state.ui = {}; f.setSnapshot(docker({ applied: dockerPolicy({ followPublicIp: true }), publicIp: { offered: true, follow: true, origin: null, errorCode: 'server_access_public_ip_conflict' } }));
  assert.match(f.find('[data-access-follow-conflict]').textContent, /다른 HTTP 주소가 하나 있어야 하고/);
  // The help points at the switch, not at a command.
  assert.match(steps(f)[1].join('\n'), /‘공인 IP 따라가기’를 켜면/); assert.doesNotMatch(steps(f)[1].join('\n'), /multitap|쉼표/);
});
test('an install with an updater has no public IP switch and keeps its listener fields', async t => {
  const f = fixture(t);
  f.change('http.enabled', true); f.change('http.origin', 'http://broker.fixture:18490');
  assert.equal(f.find('[name="http.followPublicIp"]'), null); assert.equal(f.find('[data-access-follow]'), null);
  assert.equal(f.find('[data-access-fixed="http"]'), null); assert.equal(f.find('[data-access-fixed="https"]'), null);
  for (const name of ['http.listenHost', 'http.listenPort', 'https.listenHost', 'https.listenPort', 'https.mode']) assert.ok(f.find('[name="' + name + '"]'), name);
  assert.equal(f.find('[name="http.listenPort"]').value, '18490', 'the listener still follows the address');
  f.change('primaryOrigin', 'http://broker.fixture:18490');
  f.find('[data-access-test]').click(); await flush();
  assert.equal(Object.hasOwn(f.writes[0].body.config.http, 'followPublicIp'), false);
  assert.deepEqual(Object.keys(f.writes[0].body.config.http).sort(), ['enabled', 'listenHost', 'listenPort', 'origin']);
  // A Docker install whose settings an updater applies is the same page (its status carries no `fixed`).
  f.state.ui = {}; f.setSnapshot(response({ deploymentKind: 'docker', managed: true, supported: true, dockerPorts: [{ hostPort: 18480, containerPort: 18400, protocol: 'tcp', scheme: 'https' }] }));
  assert.ok(f.find('[name="https.listenPort"]')); assert.equal(f.find('[name="http.followPublicIp"]'), null);
});

// What the real server and updater send, not a shape only a test knows.
test('a fresh install fills the listen fields from the address the server reports, also through a dual-stack socket', t => {
  const f = fixture(t, { snapshot: { protocol: 'server-access-v1', revision: 'bootstrap', applied: null, state: 'idle', managed: true, supported: true, pending: null,
    bootstrapListener: { address: '::ffff:192.0.2.10', port: 18480 } } });
  for (const kind of ['https', 'http']) { assert.equal(f.find(`[name="${kind}.listenHost"]`).value, '192.0.2.10'); assert.equal(f.find(`[name="${kind}.listenPort"]`).value, '18480'); }
  // The HTTP-only first setup passes the page's own checks (the listen fields were empty: three errors).
  f.change('https.enabled', false); f.change('http.enabled', true); f.change('http.origin', 'http://192.0.2.10:18480'); f.change('primaryOrigin', 'http://192.0.2.10:18480');
  f.find('[data-access-test]').click();
  assert.equal(f.find('.server-access-error'), null);
  assert.equal(f.writes.length, 1);
  assert.equal(f.writes[0].body.config.http.listenHost, '192.0.2.10'); assert.equal(f.writes[0].body.config.https.listenPort, 18480);
});

const modernTest = { testId: '1'.repeat(64), baseRevision: rev, configHash: '2'.repeat(64), scope: 'preflight', requiresRestart: true, expiresAt: '1970-01-01T00:17:40.000Z',
  checks: [{ name: 'configuration', status: 'passed' }, { name: 'managed_installation', status: 'passed' }, { name: 'listeners', status: 'passed' }, { name: 'certificate_identity_and_key', status: 'passed' },
    { name: 'external_origin_tls_and_identity', status: 'pending', code: 'server_access_pending_until_apply' }] };
test('the checks the updater really returns have their own names', async t => {
  const f = fixture(t, { handler: async () => modernTest });
  await testedHttp(f);
  assert.deepEqual([...f.document.querySelectorAll('.server-access-checks li span')].map(node => node.textContent), ['설정 형식', '설치 방식', '수신 포트', '인증서', '외부 접속 · 적용 후 확인']);
});

test('the errors the updater really answers say what to fix', async t => {
  for (const [code, text] of [['server_access_tls_not_readable', /인증서나 개인키 파일을 읽지 못했어요/], ['server_access_tls_identity_invalid', /인증서가 외부 접속 주소나 개인키와 맞지 않거나/], ['server_access_tls_path_invalid', /\/로 시작하는 전체 경로/],
    ['server_access_privileged_listener_not_configured', /낮은 번호의 포트/], ['server_access_listen_host_invalid', /IP 주소로 입력하세요/], ['server_access_listener_host_unavailable', /서버에 없는 IP/],
    ['server_access_revision_changed', /다른 관리자가 설정을 바꿨어요/], ['server_access_test_required', /검사 시간이 지났어요/], ['server_access_recovery_required', /적용 결과나 복원을 확인하지 못했어요/]]) {
    const f = fixture(t, { handler: async () => { const error = Error('요청을 처리하지 못했어요. 입력과 서버 상태를 확인하세요.'); error.code = code; throw error; } });
    await testedHttp(f);
    assert.match(f.find('.server-access-error').textContent, text, code);
  }
});

const savedHttp = async f => { await testedHttp(f); f.find('[data-access-save]').click(); await f.dialogs[0].action(); return f.writes[0].body.config; };
test('a save the server stored in its own spelling ends as applied, not as another administrator\'s change', async t => {
  const f = fixture(t, { handler: async path => path.endsWith('/test') ? modernTest : { accepted: true, operationId: 'matching-operation', state: 'waiting_idle' } });
  f.change('http.listenHost', 'localhost');
  const desired = await savedHttp(f);
  assert.equal(desired.http.listenHost, 'localhost');
  // update/server-access-policy.mjs host(): localhost is stored as 127.0.0.1.
  const stored = { ...desired, http: { ...desired.http, listenHost: '127.0.0.1' } };
  f.setSnapshot(response({ revision: nextRev, applied: stored, state: 'applied', operationId: 'matching-operation' }));
  assert.match(f.find('[data-access-draft-status]').textContent, /적용된 것을 확인/);
  assert.doesNotMatch(f.document.body.textContent, /다른 관리자가/);
  assert.equal(f.find('[name="http.listenHost"]').value, '127.0.0.1');
  assert.equal(f.find('[name="http.origin"]').disabled, false, 'the form opens again');
});

test('an accepted save is no unsaved edit; a failed one is again; no answer during the restart is not an error', async t => {
  const f = fixture(t, { handler: async path => path.endsWith('/test') ? modernTest : { accepted: true, operationId: 'matching-operation', state: 'waiting_idle' } });
  const desired = await savedHttp(f);
  assert.equal(f.state.ui.serverAccess.dirty, false, 'leaving the page does not ask about unsaved changes (app.js)');
  assert.doesNotMatch(f.find('.server-access-editor .panel-head').textContent, /편집 중/);
  const pending = state => ({ protocol: 'server-access-v1', revision: rev, config: config(), state, pendingConfig: desired, operationId: 'matching-operation', applySupported: true, managementOrigin: 'https://console.fixture' });
  f.setSnapshot(pending('applying'));
  assert.equal(f.find('[data-access-test-status]').hidden, true, 'not "test, then save" over the passed checks');
  // The address this page was opened on is not in the saved list: the new one is offered.
  assert.equal(f.find('[data-access-moved] a').getAttribute('href'), 'http://broker.fixture:18490/admin/');
  const answer = f.ctx.resource; f.ctx.resource = () => ({ ...answer(), error: '서버에 연결하지 못했어요.' }); f.redraw();
  assert.match(f.find('[data-access-restarting]').textContent, /다시 시작되고 있어요/);
  assert.doesNotMatch(f.find('.server-access-editor').textContent, /최신 상태를 확인하지 못했어요/);
  f.ctx.resource = answer;
  f.setSnapshot(response({ state: 'failed_rolled_back', operationId: 'matching-operation', errorCode: 'server_access_health_failed' }));
  assert.equal(f.state.ui.serverAccess.dirty, true, 'the edits are still in the form and can be saved again');
  assert.equal(f.find('[data-access-moved]'), null);
});

test('집 밖에서 접속하려면 holds only its steps after an edit (no "null" where the UDP line would be)', t => {
  const f = fixture(t);
  f.change('http.enabled', true); f.change('http.origin', 'http://192.0.2.10:18480');
  assert.equal([...f.find('[data-access-outside-steps]').childNodes].every(node => node.nodeType === 1), true);
  assert.doesNotMatch(f.find('[data-access-outside-steps]').textContent, /null/);
});
test('a Docker Compose install shows whether picture and sound go over UDP and where apps and Hosts send, whatever the switch says', t => {
  const derived = { running: true, port: 18481, derivedPort: 28481, media: 'on', relays: { host: [], client: [] }, hostRoute: null };
  const applied = dockerPolicy({ extraOrigins: ['http://203.0.113.7:8080', 'http://[2001:db8::10]:18480'] }, { enabled: true, origin: 'https://vdi.example.com', listenPort: 18480 });
  const f = fixture(t, { snapshot: docker({ applied, udpMedia: derived }) });
  const panel = () => f.find('.server-access-udp'), state = () => f.find('[data-access-udp]');
  assert.equal(panel().querySelector('h2').textContent, 'UDP 화면 전송');
  assert.equal(state().getAttribute('data-access-udp'), 'derived');
  assert.equal(state().textContent, '쓰고 있어요.\n앱과 Host는 자기가 접속한 주소의 UDP 28481 포트로 보내요.');
  // One endpoint per address in use, on the published UDP port; an IPv6 address has none.
  assert.equal(f.find('[data-access-udp-endpoints]').textContent, '보내는 곳 · vdi.example.com:28481, 192.0.2.10:28481, 203.0.113.7:28481');
  assert.equal(f.find('[data-access-udp-router]').textContent, '공유기 포트 포워딩 · UDP 28481 → 이 서버\n안 하면 밖에서는 화면이 TCP로 가요.');
  assert.equal(panel().querySelector('input,textarea,select,button'), null, 'a server that reports no switch shows none');
  // The router note of 공인 IP 따라가기 names the same published port.
  f.change('http.followPublicIp', true);
  assert.match(f.find('[data-access-follow-router]').textContent, /TCP 18480, UDP 28481 → 이 서버/);

  const lists = { host: [{ host: '192.0.2.10', port: 18481 }, { host: 'vdi.example.com', port: 18481 }], client: [{ host: 'vdi.example.com', port: 18481 }] };
  for (const [udpMedia, kind, text] of [
    // MULTITAP_UDP_MEDIA decides while it is set.
    [{ ...derived, media: 'desktops' }, 'derived', '서버 .env의 MULTITAP_UDP_MEDIA에 정한 데스크톱만 UDP로 보내요.'],
    [{ ...derived, media: 'off' }, 'derived-off', '주소는 정해져 있지만 화면은 UDP로 보내지 않아요.\n서버 .env의 MULTITAP_UDP_MEDIA를 확인하세요.'],
    // Relay lists from .env.
    [{ ...derived, derivedPort: null, relays: lists }, 'configured', '쓰고 있어요.\n서버의 .env에 정한 주소로 보내요.'],
    [{ ...derived, derivedPort: null, relays: lists, media: 'off' }, 'configured', '주소는 정해져 있지만 화면은 UDP로 보내지 않아요.\n서버 .env의 MULTITAP_UDP_MEDIA를 확인하세요.'],
    // Nothing to send to: no published UDP port told, one list only, the relay not running, or no relay at all.
    [{ ...derived, derivedPort: null }, 'off', '쓰지 않아요. 화면은 TCP로 가요.\n서버의 Docker 설정에서 UDP 포트를 확인하세요.'],
    [{ ...derived, derivedPort: null, relays: { ...lists, client: [] } }, 'off', null], [{ ...derived, running: false }, 'off', null], [null, 'off', null], [undefined, 'off', null],
  ]) {
    f.state.ui = {}; f.setSnapshot(docker({ applied, udpMedia }));
    assert.equal(state().getAttribute('data-access-udp'), kind.replace('-off', ''), JSON.stringify(udpMedia));
    if (text) assert.equal(state().textContent, text);
    assert.equal(!!f.find('[data-access-udp-router]'), kind.startsWith('derived'));
    // Turned off, the relay still answers: where a session would send, and the port to forward, stay in view.
    if (kind === 'derived-off') assert.equal(f.find('[data-access-udp-endpoints]').textContent, '보내는 곳 · vdi.example.com:28481, 192.0.2.10:28481, 203.0.113.7:28481');
    if (kind === 'configured') assert.equal(f.find('[data-access-udp-endpoints]').textContent, 'Host가 보내는 곳 · 192.0.2.10:18481, vdi.example.com:18481\n앱이 보내는 곳 · vdi.example.com:18481');
    if (kind === 'off') assert.equal(f.find('[data-access-udp-endpoints]'), null);
  }
  // An install with an updater keeps its page: no such panel, whatever the server reports.
  f.state.ui = {}; f.setSnapshot(response({ udpMedia: { ...derived, derivedPort: null, relays: lists } }));
  assert.equal(panel(), null);
});

test('프록시가 이 서버에 있어요 names the container\'s gateway as a trusted proxy only when it is pressed', async t => {
  const f = fixture(t, { snapshot: docker({ gateway: '172.18.0.1' }) });
  const button = () => f.find('[data-access-proxy-here]'), field = () => f.find('[name="trustedProxies"]');
  assert.equal(button().textContent, '프록시가 이 서버에 있어요'); assert.equal(button().disabled, false);
  assert.match(button().closest('div').textContent, /이 서버의 프록시가 들어오는 주소\(172\.18\.0\.1\)를 목록에 넣어요/);
  assert.equal(field().value, '127.0.0.1\n::1', 'nothing is added unasked');
  assert.equal(f.find('[data-access-save]').disabled, true);
  // The HTTPS card points at the button instead of naming an address to look up.
  f.change('https.enabled', true); f.change('https.origin', 'https://vdi.example.com');
  assert.match(f.find('[data-access-fixed="https"]').textContent, /아래 ‘신뢰할 프록시 IP’에서 ‘프록시가 이 서버에 있어요’를 누르세요/);
  button().click();
  assert.equal(field().value, '127.0.0.1\n::1\n172.18.0.1');
  assert.equal(button().disabled, true, 'once'); assert.equal(button().title, '이미 목록에 있어요.');
  f.find('[data-access-test]').click(); await flush();
  assert.deepEqual(f.writes[0].body.config.trustedProxies, ['127.0.0.1', '::1', '172.18.0.1']);
  // Typed by hand first: the button has nothing to add.
  f.state.ui = {}; f.setSnapshot(docker({ gateway: '172.18.0.1', applied: { ...dockerPolicy(), trustedProxies: ['172.18.0.1'] } }));
  assert.equal(button().disabled, true);
  // Not offered where the server could not read a gateway, where what it reports is no address, or on an install with an updater.
  for (const snapshot of [docker(), docker({ gateway: null }), docker({ gateway: '172.18.0.0/16' }), response({ gateway: '172.18.0.1' })]) {
    f.state.ui = {}; f.setSnapshot(snapshot);
    assert.equal(button(), null); assert.ok(field(), 'the field itself is always there');
  }
});

test('a server that started without its saved policy says so on the page, where the addresses are set again', t => {
  const f = fixture(t, { snapshot: docker({ applied: null, policyUnreadable: 'server_access_state_invalid' }) });
  assert.equal(f.find('[data-access-unreadable]').textContent, '저장된 접속 설정을 읽지 못해, 서버가 관리자 페이지만 연 채로 켜졌어요.\n주소를 다시 정해 저장하세요. 앱과 Host는 그 뒤에 다시 연결돼요.');
  assert.equal(f.find('[data-access-unreadable]').getAttribute('role'), 'alert');
  assert.equal(f.find('[name="http.origin"]').value, 'http://192.0.2.10:18480', 'the address the console is open at is the suggestion');
  assert.ok(f.find('[data-access-test]')); assert.ok(f.find('[data-access-save]'));
  for (const snapshot of [docker(), docker({ applied: null }), docker({ policyUnreadable: '' }), response()]) { f.state.ui = {}; f.setSnapshot(snapshot); assert.equal(f.find('[data-access-unreadable]'), null); }
});

test('a save after a test that could not try the address itself says so in its confirmation; one whose addresses were reached does not', async t => {
  const f = fixture(t, { handler: async path => path.endsWith('/test') ? modernTest : { accepted: true, operationId: 'fixture-operation', state: 'waiting_idle' } });
  await testedHttp(f);
  f.find('[data-access-save]').click();
  const warned = f.dialogs[0].fields.map(field => field.node).filter(node => node?.hasAttribute?.('data-access-save-unverified'));
  assert.equal(warned.length, 1);
  assert.equal(warned[0].textContent, '주소는 저장한 뒤에 확인해요.\n닿지 않으면 이전 설정으로 돌아가요.');
  // The server tried the addresses at the test: nothing is left unchecked, and the result names them.
  const reached = { ...modernTest, checks: [{ name: 'configuration', status: 'passed' }, { name: 'managed_installation', status: 'passed' }, { name: 'address', origin: 'http://broker.fixture:18490', status: 'passed', code: 'server_access_address_reached' }] };
  const g = fixture(t, { handler: async path => path.endsWith('/test') ? reached : { accepted: true, operationId: 'fixture-operation', state: 'waiting_idle' } });
  await testedHttp(g);
  assert.equal(g.find('[data-access-test-status]').textContent, '서버가 주소에 닿는 것을 확인했어요.\n저장할 수 있어요.');
  assert.deepEqual([...g.document.querySelectorAll('.server-access-checks li span')].map(node => node.textContent), ['설정 형식', '설치 방식', 'http://broker.fixture:18490이 서버로 연결돼요.']);
  g.find('[data-access-save]').click();
  assert.equal(g.dialogs[0].fields.some(field => field.node?.hasAttribute?.('data-access-save-unverified')), false);
});

test('an address check the page cannot read is not shown as a result: its address and where it came from are bounded', async t => {
  for (const check of [{ name: 'address', origin: 'h'.repeat(3000), status: 'failed', code: 'server_access_address_closed' }, { name: 'address', origin: 'https://vdi.example.com', peer: '<b>172.18.0.1</b>', status: 'failed', code: 'server_access_address_proxy_untrusted' }]) {
    const f = fixture(t, { handler: async () => ({ ...modernTest, testId: undefined, checks: [{ name: 'configuration', status: 'passed' }, check] }) });
    await testedHttp(f);
    assert.equal(f.find('.server-access-error').textContent, '검사 응답을 확인하지 못했어요. 다시 검사하세요.');
    assert.equal(f.find('[data-access-save]').disabled, true);
  }
  // A proxy the server does not trust is named, with one button that lists it and tests again.
  const f = fixture(t, { handler: async () => ({ ...modernTest, testId: undefined, checks: [{ name: 'configuration', status: 'passed' }, { name: 'address', origin: 'https://vdi.example.com', peer: '172.18.0.1', status: 'failed', code: 'server_access_address_proxy_untrusted' }] }) });
  await testedHttp(f);
  assert.equal(f.find('[data-access-address-check="failed"]').textContent, 'https://vdi.example.com프록시(172.18.0.1)가 ‘신뢰할 프록시 IP’에 없어요.172.18.0.1 추가하고 다시 테스트');
  assert.match(f.find('[data-access-test-status]').textContent, /닿지 못했어요\.\n저장하지 않았고, 지금 연결은 그대로예요\./);
  assert.equal(f.find('[data-access-save]').disabled, true);
  assert.equal(f.find('[name="trustedProxies"]').value, '127.0.0.1', 'nothing is added unasked');
});

test('a proxy the test came through is listed and tested again with one press, and only that address', async t => {
  const untrusted = { ...modernTest, testId: undefined, checks: [{ name: 'configuration', status: 'passed' }, { name: 'address', origin: 'https://remote.example.com', peer: '10.20.30.40', status: 'failed', code: 'server_access_address_proxy_untrusted' }] };
  const reached = { ...modernTest, checks: [{ name: 'configuration', status: 'passed' }, { name: 'address', origin: 'https://remote.example.com', status: 'passed', code: 'server_access_address_reached' }] };
  const answers = [untrusted, reached];
  const f = fixture(t, { handler: async path => path.endsWith('/test') ? answers.shift() : { accepted: true, operationId: 'fixture-operation', state: 'waiting_idle' } });
  await testedHttp(f);
  const trust = f.find('[data-access-proxy-trust]');
  assert.equal(trust.textContent, '10.20.30.40 추가하고 다시 테스트'); assert.equal(trust.disabled, false);
  trust.click(); await flush();
  const tests = f.writes.filter(write => write.path.endsWith('/test'));
  assert.equal(tests.length, 2);
  assert.deepEqual(tests[1].body.config.trustedProxies, ['127.0.0.1', '10.20.30.40']);
  assert.equal(f.find('[name="trustedProxies"]').value, '127.0.0.1\n10.20.30.40');
  assert.equal(f.find('[data-access-proxy-trust]'), null, 'nothing left to add');
  assert.equal(f.find('[data-access-save]').disabled, false, 'the second test can be saved');
  // Other reasons offer no such button.
  const g = fixture(t, { handler: async () => ({ ...modernTest, testId: undefined, checks: [{ name: 'address', origin: 'https://remote.example.com', peer: '10.20.30.40', status: 'failed', code: 'server_access_address_proxy_headers' }] }) });
  await testedHttp(g);
  assert.equal(g.find('[data-access-proxy-trust]'), null);
});

// ---- the switch for picture and sound over UDP ----
const udpRelay = (extra = {}) => ({ running: true, port: 18481, derivedPort: 18481, media: 'off', sessions: 0, bindings: 0, relays: { host: [], client: [] }, hostRoute: null, ...extra });
const udpSwitch = (extra = {}) => ({ saved: false, relay: true, env: null, ...extra });

test('UDP로 화면과 소리 보내기: off until an administrator turns it on, said honestly, and saved by a test and a save that end nobody\'s connection', async t => {
  const answers = { test: null };
  const f = fixture(t, { snapshot: docker({ udpMedia: udpRelay(), udpSwitch: udpSwitch() }), handler: async path => path.endsWith('/test') ? answers.test : { accepted: true, operationId: 'switch-operation', state: 'waiting_idle' } });
  const box = () => f.find('[name="udpMedia"]');
  assert.equal(f.find('.server-access-udp .server-access-switch span').textContent, 'UDP로 화면과 소리 보내기');
  assert.equal(box().getAttribute('role'), 'switch'); assert.ok(!box().checked, 'off on an install that never said'); assert.equal(box().disabled, false);
  assert.equal(f.find('[data-access-udp]').textContent, '꺼져 있어요. 화면과 소리는 TCP로 가요.\nUDP 포트에 닿는지는 켜기 전에 아래에서 볼 수 있어요.');
  assert.equal(f.find('[data-access-udp-help]').textContent, '패킷이 조금 빠져도 화면이 멈추지 않게 하려는 전송 방식이에요.\n새 기능이고, 아직 실제로 오래 써 보지 않았어요. UDP가 닿지 않는 연결은 TCP로 가요.');
  assert.equal(f.find('[data-access-udp-effect]').textContent, '켜거나 꺼도 서버를 다시 시작하지 않고, 연결도 끊기지 않아요.\n켜면 새로 연결하는 세션부터 UDP로 보내고, 끄면 연결 중인 세션도 TCP로 돌아갈 수 있어요.');
  // What is needed for it stays in view while it is off: where a session would send, the port to forward, and who reaches it now.
  assert.equal(f.find('[data-access-udp-endpoints]').textContent, '보내는 곳 · 192.0.2.10:18481');
  assert.equal(f.find('[data-access-udp-router]').textContent, '공유기 포트 포워딩 · UDP 18481 → 이 서버\n안 하면 밖에서는 화면이 TCP로 가요.');
  assert.equal(f.find('[data-access-udp-reach]').textContent, '세션이 연결되면 UDP 포트에 닿는지 여기에 보여요.');
  assert.equal(f.find('[data-access-save]').disabled, true);
  // Turned on: an edit like any other, tested and saved.
  f.change('udpMedia', true);
  assert.equal(f.find('[data-access-draft-status]').textContent, '편집 중 · 아직 적용하지 않았어요.');
  answers.test = { testId: '1'.repeat(64), baseRevision: rev, configHash: '2'.repeat(64), scope: 'preflight', requiresRestart: true, addressesUnchanged: true, expiresAt: '1970-01-01T00:17:40.000Z',
    checks: [{ name: 'configuration', status: 'passed' }, { name: 'managed_installation', status: 'passed' }] };
  f.find('[data-access-test]').click(); await flush();
  assert.equal(f.writes[0].body.config.udpMedia, true); assert.deepEqual({ ...f.writes[0].body.config, udpMedia: undefined }, { ...dockerPolicy(), udpMedia: undefined }, 'the addresses as they are');
  assert.equal(f.find('[data-access-test-status]').textContent, '저장할 수 있어요.\n주소는 그대로라 서버를 다시 시작하지 않고, 연결도 끊기지 않아요.');
  f.find('[data-access-save]').click();
  assert.equal(f.dialogs[0].options.description, '주소는 그대로예요.\n서버를 다시 시작하지 않고, 연결도 끊기지 않아요.');
  assert.equal(f.dialogs[0].options.danger, false); assert.deepEqual(f.dialogs[0].fields, [], 'nobody is disconnected: nobody to tell first');
  await f.dialogs[0].action(); await flush();
  assert.equal(f.writes[1].body.config.udpMedia, true);
  assert.equal(f.find('[data-access-waiting]'), null, 'no "ending connections"'); assert.equal(f.find('[data-access-draft-status]').textContent, '새 설정을 적용하고 있어요. 완료 상태를 기다려 주세요.');
  // Applied: the server reports it in use, and the page is not "edited" any more.
  f.setSnapshot(docker({ revision: nextRev, applied: { ...dockerPolicy(), udpMedia: true }, state: 'applied', operationId: 'switch-operation', udpMedia: udpRelay({ media: 'on', sessions: 2, bindings: 3 }), udpSwitch: udpSwitch({ saved: true }) }));
  assert.equal(f.find('[data-access-draft-status]').textContent, '새 설정이 적용된 것을 확인했어요.'); assert.equal(box().checked, true);
  assert.equal(f.find('[data-access-udp]').textContent, '쓰고 있어요.\n앱과 Host는 자기가 접속한 주소의 UDP 18481 포트로 보내요.');
  assert.equal(f.find('[data-access-udp-reach]').textContent, '지금 세션 2개 · UDP 포트에 닿은 쪽 3곳\n한 세션은 앱과 Host 두 쪽이 모두 닿아야 UDP로 보낼 수 있어요.');
  // Off again goes without the key, as an install that never had it.
  f.change('udpMedia', false);
  answers.test = { ...answers.test, testId: '3'.repeat(64), baseRevision: nextRev };
  f.find('[data-access-test]').click(); await flush();
  assert.equal(Object.hasOwn(f.writes[2].body.config, 'udpMedia'), false);
});

test('UDP로 화면과 소리 보내기 fixed by .env: shown with its value and not to be changed here; no switch where the relay does not run or the install has an updater', t => {
  for (const [env, media, checked, value] of [['on', 'on', true, 'on'], ['off', 'off', false, 'off'], ['desktops', 'desktops', true, '데스크톱 목록']]) {
    const f = fixture(t, { snapshot: docker({ udpMedia: udpRelay({ media }), udpSwitch: udpSwitch({ env, saved: env === 'off' }) }) });
    const box = f.find('[name="udpMedia"]');
    assert.equal(box.disabled, true, env); assert.equal(!!box.checked, checked, env);
    assert.equal(f.find('[data-access-udp-fixed]').textContent, '서버 .env의 MULTITAP_UDP_MEDIA(' + value + ')로 정해져 있어요.\n여기서 바꾸려면 .env에서 그 줄을 지우세요.', env);
    assert.equal(f.find('[data-access-udp-effect]'), null, env); assert.ok(f.find('[data-access-udp-help]'), env);
    if (env === 'off') assert.equal(f.find('[data-access-udp]').textContent, '꺼 두었어요. 화면은 TCP로 가요.\n서버 .env의 MULTITAP_UDP_MEDIA가 off예요.');
    // The saved value is not sent as an edit of this page.
    assert.equal(f.find('[data-access-save]').disabled, true, env);
  }
  // No relay (no UDP port in the container), a relay that is down, or a switch report the page cannot read: the state, and no switch.
  for (const snapshot of [docker({ udpMedia: null, udpSwitch: udpSwitch({ relay: false }) }), docker({ udpMedia: udpRelay({ running: false }), udpSwitch: udpSwitch() }), docker({ udpMedia: udpRelay(), udpSwitch: { saved: 'yes', relay: true, env: null } }),
    docker({ udpMedia: udpRelay(), udpSwitch: udpSwitch({ env: 'sometimes' }) }), docker({ udpMedia: udpRelay({ derivedPort: null }), udpSwitch: udpSwitch() })]) {
    const f = fixture(t, { snapshot });
    assert.equal(f.find('[name="udpMedia"]'), null); assert.ok(f.find('[data-access-udp]'));
  }
  const updater = fixture(t, { snapshot: response({ udpMedia: udpRelay(), udpSwitch: udpSwitch() }) });
  assert.equal(updater.find('.server-access-udp'), null);
});
