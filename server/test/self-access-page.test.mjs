// The 서버 접속 설정 page of a Docker Compose install, from 연결 테스트 to the
// end of a save: the real page (admin/js/server-access.js, in the DOM the other
// page tests use) asks the real in-process client (src/self-access.mjs) through
// the real fence (src/access-runtime.mjs), on real files in a temporary data
// folder. A "restart" is a new load of that folder, and while the server is
// down the page's requests fail as a browser's do. A page is open at an
// address: its requests are let in, or refused, by the server's own judgment
// of that address (src/request-origin.mjs, and the door a save waiting for its
// proof opens). What an address answers is given here; no listener and no network.
import test from 'node:test';
import assert from 'node:assert/strict';
import { mkdtemp, rm, readdir } from 'node:fs/promises';
import { readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { parseHTML } from 'linkedom';
import { el } from '../admin/js/dom.js';
import { render, saveOverdueMs, outsideLink } from '../admin/js/server-access.js';
import { configuration, selfAccessStorage } from '../src/config.mjs';
import { SelfAccess, accessProbeProtocol, clearSelfJournal } from '../src/self-access.mjs';
import { AccessRuntime } from '../src/access-runtime.mjs';
import { resolveRequestAccess } from '../src/request-origin.mjs';
import { main as accessCli } from '../src/access-cli.mjs';
import { serverAccessHash } from '../update/server-access-policy.mjs';

const lan = 'http://192.0.2.10:18480', secure = 'https://vdi.example.com';
const readAccessFile = path => { try { return readFileSync(path, 'utf8'); } catch (error) { if (error.code === 'ENOENT') return null; throw error; } };
const quiet = () => ({ protocol: 'server-access-activity-v1', fence: true, connections: { people: 0, sessions: 0, consoles: 0 },
  activity: { protocol: 'activity-v1', sessions: { total: 0, stream: 0, attached: 0, tickets: 0 },
    channels: { clipboard: 0, clipboardTransfers: 0, drive: 0, driveShares: 0, driveRequests: 0, usb: 0, usbDevices: 0, print: 0, printTransfers: 0 },
    operations: { display: 0, sessionCleanup: 0, hostUpdates: 0, power: 0 }, hosts: { connected: 0 }, live: { channels: 0 }, sinceActivityMs: 1000, uptimeMs: 5000 } });
const settingsOf = directory => ({ MULTITAP_HOST: '0.0.0.0', MULTITAP_PORT: '18480', MULTITAP_DEPLOYMENT_KIND: 'docker', MULTITAP_DATA_DIR: directory, MULTITAP_PUBLISHED_PORT: '0.0.0.0:18480', MULTITAP_SERVER_ADDRESS: '192.0.2.10',
  MULTITAP_HTTP_ORIGIN: lan, MULTITAP_ALLOW_INSECURE_HTTP: '1' });

// One process of the server on the data folder, as index.mjs and http.mjs put
// it together: the access client, and the fence the console's requests take.
// `reach`: what 연결 테스트 finds at an address. `healthy`: what it answers after the restart.
// `end()`: the process is gone, and whatever it was waiting for is never continued.
async function boot(directory, { reach = true, healthy = true, activity = quiet, ...options } = {}) {
  const settings = settingsOf(directory), config = configuration(settings, { readAccessFile }), run = { settings, config, exits: 0, ended: 0, tried: [], probes: [], records: [], over: false };
  const publicRequest = async (current, origin) => { run.probes.push(origin); return (typeof healthy === 'function' ? healthy(origin) : healthy) ? { service: 'multitap-broker', status: 'ok', authentication: 'id-password', version: '1.1.2',
    access: { protocol: 'server-access-v1', revision: serverAccessHash(current), installationId: config.accessInstallationId, bootstrapOnly: false, origin } } : null; };
  const probeRequest = async (origin, nonce) => { run.tried.push(origin); const found = typeof reach === 'function' ? reach(origin, nonce) : reach; return found === true ? { answer: { protocol: accessProbeProtocol, nonce, admitted: true } } : found; };
  run.access = SelfAccess.from(config, { exit: () => { run.exits++; }, load: () => configuration(settings, { readAccessFile }), publicRequest, probeRequest,
    sleep: () => run.over ? new Promise(() => {}) : new Promise(resolve => setImmediate(resolve)), pollMs: 1, verifyAttempts: 2, verifyDelayMs: 1, ...options });
  run.end = () => { run.over = true; };
  await run.access.initialize();
  run.runtime = new AccessRuntime({ client: run.access, config: config.accessConfig, revision: config.accessRevision, installationId: config.accessInstallationId, initialOperationId: run.access.startupOperationId });
  run.access.attach({ activity, endConnections: () => { run.ended++; run.runtime.connectionsEnding(); return { protocol: 'server-access-connections-v1', fence: true, ended: { people: 0, sessions: 0, consoles: 0 } }; },
    hold: lease => run.runtime.hold(lease), release: lease => run.runtime.release(lease), blocked: () => run.runtime.blocked, settle: () => run.runtime.status({ managementOrigin: null }), record: (...row) => run.records.push(row) });
  return run;
}
// A browser's request to an address, as it reaches this container's listener: an https one through a proxy on this machine.
const requestAt = origin => { const url = new URL(origin), secured = url.protocol === 'https:';
  return { rawHeaders: ['Host', url.host], headers: { host: url.host, ...(secured ? { 'x-forwarded-proto': 'https' } : {}) }, socket: { remoteAddress: '127.0.0.1', localPort: 18480, encrypted: false } }; };

// The page in a browser that stays open across the server's restarts, at the
// server's own address; `open(address)` is another browser at another address
// (`code`: opened by the link the first page shows).
async function page(t, options) {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-access-page-'));
  const window = parseHTML('<!doctype html><html><body></body></html>');
  const previous = Object.fromEntries(['document', 'Node'].map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  Object.defineProperty(globalThis, 'document', { configurable: true, value: window.document });
  Object.defineProperty(globalThis, 'Node', { configurable: true, value: window.Node });
  // `skew` moves the server's clock, `clock` is the browsers'.
  const world = { directory, skew: 0, clock: Date.now(), down: false, servers: [], views: [] };
  const start = async more => { const server = await boot(directory, { now: () => Date.now() + world.skew, ...more }); world.servers.push(server); world.server = server; };
  await start(options);
  t.after(async () => {
    for (const view of world.views) { const model = view.state.ui.serverAccess; clearTimeout(model?.expiryTimer); clearTimeout(model?.overdueTimer); clearInterval(model?.ticker); clearInterval(model?.poller); }
    for (const server of world.servers) { server.end(); server.runtime.close(); server.access.close(); }
    for (const [key, descriptor] of Object.entries(previous)) { if (descriptor) Object.defineProperty(globalThis, key, descriptor); else delete globalThis[key]; }
    await rm(directory, { recursive: true, force: true });
  });
  const open = (origin, { code = null } = {}) => {
    const container = window.document.createElement('section'); window.document.body.append(container);
    const f = { origin, dialogs: [], notices: [], state: { user: { id: 'owner' }, ui: code ? { accessConfirmCode: code } : {} }, entry: { value: null, error: '', code: null }, loseNextRead: false };
    // http.mjs and admin-http.mjs: the request is let in at its address or refused, then goes to the fence.
    f.api = async (path, method = 'GET', body) => {
      if (world.down) throw Error('서버에 연결하지 못했어요.');
      const req = requestAt(origin), server = world.server;
      let access;
      try { access = resolveRequestAccess(req, server.config.accessConfig); }
      catch (error) { access = server.access.provisionalAccess(req); if (!access) throw Object.assign(Error('요청을 처리하지 못했어요.'), { code: error.code, status: 403 }); }
      const context = { managementOrigin: access.origin }, admitted = config => { try { return resolveRequestAccess(req, config).origin; } catch { return null; } };
      if (path === '/server-access') return { ...await server.runtime.status({ ...context, listener: null, deploymentKind: 'docker', udpPort: null, udpMedia: null }), connections: { people: 0, sessions: 0, consoles: 0 } };
      if (path === '/server-access/test') return server.runtime.test(body, context);
      if (path === '/server-access/save') return server.runtime.save(body, context, { actor: 'owner', admitted });
      if (path === '/server-access/cancel') return server.runtime.cancel(body.operationId);
      if (path === '/server-access/confirm') return server.runtime.confirm(body, context, { actor: 'owner', admitted });
      throw Error('unexpected ' + path);
    };
    const ctx = { state: f.state, now: () => world.clock, setupRequired: () => false, link: (text, name) => el('a', { href: '#/' + name }, text), resource: () => f.entry, value: () => f.entry.value,
      pageHeader: ({ title }) => el('header', {}, el('h1', {}, title)), render: () => container.replaceChildren(...render(ctx)), api: f.api,
      modal: (title, fields, action, more) => f.dialogs.push({ title, fields, action, options: more }), load: () => f.poll(), notice: (text, error = false) => f.notices.push({ text, error }) };
    // app.js load(): the answer replaces the last one; a failed read keeps it and says so.
    f.poll = async () => {
      try { if (f.loseNextRead) { f.loseNextRead = false; throw Error('서버에 연결하지 못했어요.'); } f.entry.value = JSON.parse(JSON.stringify(await f.api('/server-access'))); f.entry.error = ''; f.entry.code = null; }
      catch (error) { f.entry.error = error.message; f.entry.code = typeof error.code === 'string' ? error.code : null; }
      ctx.render();
    };
    f.redraw = () => ctx.render();
    f.find = selector => container.querySelector(selector);
    f.all = selector => [...container.querySelectorAll(selector)];
    f.change = (name, value) => { const input = f.find('[name="' + name + '"]'); assert.ok(input, name); if (input.type === 'checkbox') input.checked = value; else input.value = value; input.dispatchEvent(new window.Event(input.type === 'checkbox' ? 'change' : 'input')); };
    f.flush = async () => { for (let i = 0; i < 12; i++) await new Promise(resolve => setImmediate(resolve)); };
    f.status = () => f.find('[data-access-draft-status]')?.textContent ?? '';
    f.waiting = () => f.find('[data-access-waiting]')?.textContent ?? null;
    f.testText = () => f.find('[data-access-test-status]')?.textContent ?? '';
    f.locked = () => f.find('[name="https.origin"]')?.disabled === true || f.find('[name="http.enabled"]')?.disabled === true;
    // What the owner did: an address in the HTTPS card, 연결 테스트, then 변경 저장.
    f.typeHttps = async () => { f.change('https.enabled', true); f.change('https.origin', secure); f.find('[data-access-test]').click(); await f.flush(); };
    f.save = async () => { f.find('[data-access-save]').click(); assert.equal(f.dialogs.length, 1, 'the one confirmation'); await f.dialogs[0].action(); await f.flush(); };
    // The other way: 직접 확인하고 저장. Answers the code the page then shows.
    f.saveOutside = async () => {
      const before = f.dialogs.length; f.find('[data-access-save-outside]').click(); assert.equal(f.dialogs.length, before + 1, 'the one confirmation');
      await f.dialogs.at(-1).action(); await f.flush();
      return /\d{6}/.exec(f.find('[data-access-outside-code]')?.textContent ?? '')?.[0] ?? null;
    };
    world.views.push(f);
    return f;
  };
  const f = open(lan);
  for (const key of ['directory', 'server', 'down', 'clock', 'skew']) Object.defineProperty(f, key, { get: () => world[key], set: value => { world[key] = value; } });
  f.open = open;
  f.restart = async more => { world.server.end(); await start(more); world.down = false; };
  await f.poll();
  return f;
}
const files = async directory => readdir(join(directory, 'access')).then(list => list.sort(), () => []);

test('an HTTPS address the server cannot reach: 연결 테스트 says why and what HTTPS needs, nothing is saved, and only a reason that fits "opens from outside" offers the proof from outside', async t => {
  const f = await page(t, { reach: origin => origin === secure ? { error: 'closed' } : true });
  await f.typeHttps();
  assert.deepEqual(f.server.tried.sort(), [lan, secure].sort(), 'the server tried the addresses itself');
  assert.equal(f.testText(), '서버에서는 그 주소에 닿지 못했어요.\n밖에서는 열리는 주소라면 직접 확인하고 저장할 수 있어요.');
  assert.doesNotMatch(f.testText(), /통과/, 'an address nobody reached is not "passed"');
  const rows = f.all('[data-access-address-check]').map(row => [row.getAttribute('data-access-address-check'), row.textContent]);
  assert.deepEqual(rows.find(row => row[1].startsWith(secure)), ['failed', secure + '그 주소에서 받는 곳이 없어요. 포트포워딩이나 리버스 프록시를 확인하세요.']);
  assert.deepEqual(rows.find(row => row[1].startsWith(lan)), ['passed', lan + '이 서버로 연결돼요.'], 'the address that works is told apart');
  assert.equal(f.find('[data-access-https-needs]').textContent, 'HTTPS 주소는 도메인이 리버스 프록시를 가리키고, 그 프록시가 이 서버로 넘겨야 해요.');
  // The ordinary save has nothing to save with. The other way is offered beside the result, in the page's own words.
  assert.equal(f.find('[data-access-save]').disabled, true);
  f.find('[data-access-save]').click(); assert.equal(f.dialogs.length, 0);
  const offer = f.find('[data-access-outside-offer]');
  assert.equal(offer.querySelector('p').textContent, '저장을 누르면 확인 코드가 나와요. 5분 안에 그 주소로 관리자 페이지를 열어 로그인하고 코드를 넣으면 적용돼요.\n그때까지는 아무것도 바뀌지 않아요.');
  assert.equal(f.find('[data-access-save-outside]').textContent, '직접 확인하고 저장'); assert.equal(f.find('[data-access-save-outside]').disabled, false);
  assert.doesNotMatch(f.find('.server-access-results').textContent, /multitap address/, 'no command to run instead');
  assert.equal(f.server.exits, 0); assert.equal(f.server.ended, 0); assert.equal(f.server.runtime.blocked, false);
  assert.deepEqual(await files(f.directory), [], 'nothing was written');
  await f.poll();
  assert.equal(f.entry.value.state, 'idle'); assert.equal(f.locked(), false); assert.equal(f.find('[name="https.origin"]').value, secure, 'what was typed is still there to correct');
  // Changing the address takes the offer away with the result it belonged to.
  f.change('https.origin', 'https://vdi2.example.com');
  assert.equal(f.find('[data-access-outside-offer]'), null);
  f.change('https.origin', secure);
  // Each reason in its own words. Out of reach from inside: offered. Something answering wrongly: never.
  const answers = { other_server: () => ({ answer: null }), proxy_untrusted: nonce => ({ answer: { protocol: accessProbeProtocol, nonce, admitted: false, reason: 'proxy_untrusted', peer: '172.18.0.1' } }),
    proxy_headers: nonce => ({ answer: { protocol: accessProbeProtocol, nonce, admitted: false, reason: 'proxy_headers', peer: '127.0.0.1' } }), not_admitted: nonce => ({ answer: { protocol: accessProbeProtocol, nonce, admitted: false } }) };
  for (const [reason, said, offered] of [['name_unknown', /이 이름의 주소를 찾지 못했어요/, true], ['unchecked', /서버에서 확인하지 못했어요/, true], ['no_answer', /응답이 없어요/, true],
    ['not_https', /HTTPS로 답하지 않아요/, false], ['certificate', /인증서가 이 이름과 맞지 않거나/, false], ['other_server', /다른 곳이 답하고 있어요/, false],
    ['proxy_untrusted', /프록시\(172\.18\.0\.1\)가 ‘신뢰할 프록시 IP’에 없어요/, false], ['proxy_headers', /프록시가 원래 주소를 넘겨 주지 않아요/, false], ['not_admitted', /이 서버에 닿았지만 이 주소로 받지 않았어요/, false]]) {
    await f.restart({ reach: (origin, nonce) => origin !== secure ? true : answers[reason] ? answers[reason](nonce) : { error: reason } });
    f.find('[data-access-test]').click(); await f.flush();
    assert.match(f.all('[data-access-address-check="failed"]').map(row => row.textContent).join('|'), said, reason);
    assert.equal(f.find('[data-access-save]').disabled, true, reason);
    assert.equal(!!f.find('[data-access-save-outside]'), offered, reason);
    assert.equal(f.testText(), offered ? '서버에서는 그 주소에 닿지 못했어요.\n밖에서는 열리는 주소라면 직접 확인하고 저장할 수 있어요.' : '서버가 그 주소로 연결해 봤지만 닿지 못했어요.\n저장하지 않았고, 지금 연결은 그대로예요.', reason);
  }
  // Corrected (the proxy now forwards here): the same button says it is reached, and the save opens.
  await f.restart();
  f.find('[data-access-test]').click(); await f.flush();
  assert.equal(f.testText(), '서버가 주소에 닿는 것을 확인했어요.\n저장할 수 있어요.');
  assert.equal(f.all('[data-access-address-check="failed"]').length, 0); assert.equal(f.find('[data-access-https-needs]'), null); assert.equal(f.find('[data-access-outside-offer]'), null);
  assert.equal(f.find('[data-access-save]').disabled, false);
});

test('a save whose address stops answering after the restart: each step is said, it ends by itself with the address named, and a reload still says it', async t => {
  const f = await page(t);
  await f.typeHttps();
  assert.equal(f.find('[data-access-save]').disabled, false);
  await f.save();
  assert.equal(f.waiting(), '새 설정을 적용하고 있어요. 보통 몇 초면 끝나요.'); assert.equal(f.locked(), true);
  await f.server.access.task; await f.poll();
  assert.equal(f.server.exits, 1); assert.equal(f.status(), '새 설정을 적용하고 있어요. 완료 상태를 기다려 주세요.'); assert.equal(f.waiting(), null);
  // The server is down while it restarts.
  f.down = true; await f.poll();
  assert.ok(f.find('[data-access-restarting]'), 'no answer is the restart, not an error'); assert.equal(f.locked(), true);
  // It is back with the new policy and asks itself at the new address: no answer there.
  await f.restart({ healthy: origin => origin !== secure });
  await f.poll();
  assert.equal(f.status(), '새 주소의 연결을 확인하고 있어요. 완료 상태를 기다려 주세요.'); assert.equal(f.locked(), true);
  await f.server.access.resume();
  assert.equal(f.server.exits, 1, 'the restart that loads the previous settings');
  f.down = true; await f.poll();
  await f.restart();
  await f.poll();
  const said = '서버가 ' + secure + '에 닿지 못해 이전 설정으로 돌아갔어요.\n그 주소가 이 서버로 연결되는지 확인한 뒤 다시 저장하세요.';
  assert.equal(f.status(), said);
  const notice = f.find('[data-access-draft-status]');
  assert.equal(notice.getAttribute('role'), 'alert'); assert.ok(notice.classList.contains('server-access-warning'), 'a notice, not a grey line under the form');
  assert.equal(f.waiting(), null); assert.equal(f.find('[data-access-restarting]'), null); assert.equal(f.locked(), false, 'the form opens again');
  assert.equal(f.find('[name="https.origin"]').value, secure, 'what was typed is still there to correct');
  assert.equal(f.server.runtime.blocked, false, 'the fence is given back');
  assert.equal(f.entry.value.applied.https.enabled, false, 'the settings in use are the previous ones');
  // Typing into the form does not take the reason away.
  f.change('https.origin', 'https://vdi2.example.com');
  assert.equal(f.status(), said);
  // A reload: the form is the saved settings again, and the reason is still said.
  clearTimeout(f.state.ui.serverAccess.expiryTimer); f.state.ui = {};
  await f.poll();
  assert.equal(f.status(), said); assert.equal(f.find('[data-access-draft-status]').getAttribute('role'), 'alert');
  assert.equal(f.find('[name="https.enabled"]').checked ?? false, false);
  // And after another restart of the server.
  await f.restart(); f.state.ui = {}; await f.poll();
  assert.equal(f.status(), said);
  // A day later it is still said, as a plain line: no longer an alert on every visit.
  f.clock += 25 * 3600000; f.state.ui = {}; await f.poll();
  assert.equal(f.status(), said); assert.equal(f.find('[data-access-draft-status]').getAttribute('role'), 'status');
  assert.equal(f.find('[data-access-draft-status]').classList.contains('server-access-warning'), false);
});

test('a save the server no longer has a record of: the page stops waiting, says the result is unknown and opens the form', async t => {
  const f = await page(t);
  await f.typeHttps();
  // The answer to the page's first look after the save is lost with the restart.
  f.loseNextRead = true;
  await f.save();
  assert.equal(f.waiting(), '새 설정을 적용하고 있어요. 보통 몇 초면 끝나요.');
  await f.server.access.task;
  assert.equal(f.server.exits, 1);
  // Meanwhile the settings are put back from the command line (multitap reset), which clears the record of the save.
  f.down = true;
  let printed = '';
  assert.equal(await accessCli(['reset'], { env: f.server.settings, stdout: { write: chunk => { printed += chunk; } }, configure: value => configuration(value, { readAccessFile }), storage: value => selfAccessStorage(value, { readAccessFile }) }), 0, printed);
  await f.restart();
  await f.poll();
  assert.equal(f.entry.value.operationId, undefined, 'the server reports no save at all');
  // One more look: an answer asked for before the save could still have arrived once.
  await f.poll();
  assert.equal(f.waiting(), null, 'not waiting for a result that will never be reported');
  assert.equal(f.find('.server-access-error').textContent, '저장 결과를 확인하지 못했어요.\n‘현재 적용’이 바뀌지 않았으면 다시 테스트한 뒤 저장하세요.');
  assert.equal(f.locked(), false); assert.equal(f.find('[name="https.origin"]').value, secure);
  assert.equal(f.find('[data-access-test]').disabled, false);
});

test('a save whose record is gone after it was applied: the page stops waiting and shows the settings in use', async t => {
  const f = await page(t);
  await f.typeHttps(); await f.save();
  await f.server.access.task; await f.poll();
  f.down = true; await f.poll();
  await f.restart(); await f.server.access.resume();
  assert.equal(f.server.access.status().state, 'succeeded');
  // The record of the result is lost before the page reads it.
  f.down = true; await clearSelfJournal(f.server.access.paths);
  await f.restart(); await f.poll();
  assert.equal(f.entry.value.operationId, undefined); assert.equal(f.entry.value.applied.https.origin, secure);
  assert.equal(f.waiting(), null); assert.equal(f.locked(), false); assert.equal(f.find('[data-access-restarting]'), null);
  assert.equal(f.find('.server-access-error'), null, 'the settings in use are the saved ones: nothing is unknown');
  assert.equal(f.status(), '', '"applied" is only said of a result the server reported for this save');
  assert.equal(f.find('[name="https.origin"]').value, secure); assert.match(f.find('.server-access-current').textContent, /https:\/\/vdi\.example\.com/);
});

test('a save that never reports an end: after three minutes the page stops saying "wait" and offers the reload', async t => {
  const f = await page(t);
  await f.typeHttps(); await f.save();
  await f.server.access.task; await f.poll();
  // The server does not come back.
  f.down = true; await f.poll();
  f.clock += saveOverdueMs - 1000; f.redraw();
  assert.ok(f.find('[data-access-restarting]')); assert.equal(f.find('[data-access-overdue]'), null);
  f.clock += 1000; f.redraw();
  assert.equal(f.find('[data-access-overdue]').textContent, '3분이 지나도 결과를 받지 못했어요.\n서버가 켜져 있는지 확인하고 ‘현재 설정 다시 불러오기’를 누르세요.');
  assert.equal(f.find('[data-access-overdue]').getAttribute('role'), 'alert');
  assert.equal(f.waiting(), null); assert.equal(f.find('[data-access-restarting]'), null); assert.equal(f.status(), '');
  const reload = f.all('button').find(button => button.textContent.includes('현재 설정 다시 불러오기'));
  assert.equal(reload.disabled, false, 'the way out is on the page');
  // The server is started again (its record says the save was applied but never checked: it checks now).
  await f.restart(); await f.server.access.resume();
  reload.click(); await f.flush();
  assert.equal(f.find('[data-access-overdue]'), null); assert.equal(f.entry.value.applied.https.origin, secure); assert.equal(f.locked(), false);
});

test('connections that cannot be ended: the save ends as not applied with that reason, the fence is given back and the form opens', async t => {
  // A session the server ends and still counts.
  const stuck = () => { const report = quiet(); report.connections.people = 1; report.connections.sessions = 1; Object.assign(report.activity.sessions, { total: 1, stream: 1, attached: 1 }); return report; };
  let now = Date.now();
  const f = await page(t, { activity: stuck, endWaitMs: 15000, now: () => now, sleep: async () => { now += 250; await new Promise(resolve => setImmediate(resolve)); } });
  await f.typeHttps(); await f.save();
  await f.server.access.task;
  assert.ok(f.server.ended >= 2, 'asked to end them again and again'); assert.equal(f.server.exits, 0);
  assert.equal(f.server.runtime.blocked, true, 'held until the end is read');
  await f.poll();
  assert.equal(f.entry.value.state, 'failed'); assert.equal(f.entry.value.errorCode, 'server_access_connections_busy'); assert.equal(f.entry.value.unchangedVerified, true);
  assert.equal(f.server.runtime.blocked, false, 'given back by that read');
  assert.equal(f.status(), '연결을 다 끊지 못해 적용하지 않았어요. 현재 설정은 그대로예요.\n잠시 뒤 다시 저장하세요.');
  assert.equal(f.find('[data-access-draft-status]').getAttribute('role'), 'alert');
  assert.equal(f.waiting(), null); assert.equal(f.locked(), false);
  assert.deepEqual(await files(f.directory), ['journal.json'], 'no policy was written');
});

// ---- an address the server cannot reach, proven by the administrator from outside ----
const outOfReach = { reach: origin => origin === secure ? { error: 'no_answer' } : true };
const left = f => f.find('[data-access-outside-left]')?.textContent ?? null;

test('직접 확인하고 저장: the page shows the link, the code and the time left; opened at that address and signed in, the save goes on and ends as applied', async t => {
  const f = await page(t, outOfReach);
  // Before anything: the address is no way in.
  const early = f.open(secure); await early.poll();
  assert.equal(early.entry.value, null); assert.notEqual(early.entry.error, '');
  await f.typeHttps();
  f.find('[data-access-save-outside]').click();
  assert.equal(f.dialogs[0].title, '직접 확인하고 저장할까요?'); assert.equal(f.dialogs[0].options.save, '직접 확인하고 저장');
  assert.equal(f.dialogs[0].options.description, '5분 안에 ' + secure + '에서 관리자 페이지에 로그인해 확인하세요.\n확인되면 접속 중인 연결을 끊고 적용해요. 확인하지 못하면 아무것도 바뀌지 않아요.');
  await f.dialogs[0].action(); await f.flush();
  const panel = f.find('[data-access-outside-wait]'), code = /\d{6}/.exec(f.find('[data-access-outside-code]').textContent)[0];
  assert.equal(panel.querySelector('p').textContent, '아래 주소를 밖에서 열어 로그인하고 확인 코드를 넣으세요. 휴대폰의 모바일 데이터로 여는 게 확실해요.\n확인되면 바로 적용하고, 시간 안에 못 하면 아무것도 바꾸지 않아요.');
  const link = panel.querySelector('a');
  assert.equal(link.textContent, secure); assert.equal(link.getAttribute('href'), secure + '/admin/#/server-access/confirm-' + code); assert.equal(link.getAttribute('href'), outsideLink(secure, code));
  assert.equal(f.find('[data-access-outside-origin]').getAttribute('data-access-outside-origin'), 'waiting'); assert.match(f.find('[data-access-outside-origin]').textContent, /확인을 기다려요/);
  assert.equal(f.find('[data-access-outside-code]').textContent, '확인 코드 ' + code); assert.equal(left(f), '남은 시간 5:00');
  assert.ok(f.find('[data-access-cancel]'), 'it can be withdrawn'); assert.equal(f.locked(), true);
  assert.equal(f.waiting(), null, 'nothing is being ended or applied yet, and the page does not say so'); assert.equal(f.status(), '');
  // The time left is the server's, counted on by the page.
  f.clock += 61000; f.redraw();
  assert.equal(left(f), '남은 시간 3:59');
  // Nothing has changed on the server: no fence, nobody ended, the policy in use still in use.
  assert.equal(f.server.exits, 0); assert.equal(f.server.ended, 0); assert.equal(f.server.runtime.blocked, false, 'people go on working meanwhile');
  assert.deepEqual(await files(f.directory), ['journal.json']); assert.equal(f.entry.value.applied.https.enabled, false);
  // The phone, over mobile data: the link opens the console at the new address; signed in, the page sends the code that came with it.
  const phone = f.open(secure, { code });
  await phone.poll(); await phone.flush();
  assert.equal(phone.entry.error, '', 'the address under proof leads to the console now');
  await f.server.access.task;
  assert.deepEqual(f.server.records.map(row => row[0]), ['server_access_outside_started', 'server_access_outside_confirmed']);
  assert.equal(f.server.exits, 1, 'proven: the save went on to its restart');
  await f.poll();
  assert.equal(f.find('[data-access-outside-wait]'), null); assert.equal(f.status(), '새 설정을 적용하고 있어요. 완료 상태를 기다려 주세요.');
  f.down = true; await f.poll();
  assert.ok(f.find('[data-access-restarting]'));
  // Restarted with the new policy: it asks itself only at the address it can reach.
  await f.restart({ ...outOfReach, healthy: origin => origin !== secure });
  await f.server.access.resume();
  assert.deepEqual(f.server.probes, [lan]);
  await f.poll();
  assert.equal(f.status(), '새 설정이 적용된 것을 확인했어요.'); assert.equal(f.locked(), false); assert.equal(f.entry.value.applied.https.origin, secure);
  assert.equal(f.find('[data-access-overdue]'), null);
  // And the phone is at an address of the server like any other.
  await phone.poll();
  assert.equal(phone.entry.error, ''); assert.equal(phone.entry.value.managementOrigin, secure); assert.equal(phone.entry.value.state, 'applied');
});

test('직접 확인하고 저장, not confirmed in five minutes: nothing changed and nobody was disconnected, said with the address, and the address is closed again', async t => {
  const f = await page(t, outOfReach);
  await f.typeHttps();
  const code = await f.saveOutside();
  assert.match(code, /^\d{6}$/);
  const phone = f.open(secure); await phone.poll();
  assert.equal(phone.entry.error, '');
  f.skew += 299000; await f.flush(); await f.poll();
  assert.equal(f.entry.value.state, 'awaiting_confirmation'); assert.equal(left(f), '남은 시간 0:01');
  f.skew += 1000; await f.server.access.task; await f.poll();
  const said = '5분 안에 ' + secure + '에서 확인하지 못해 적용하지 않았어요.\n현재 설정은 그대로이고, 끊긴 연결도 없어요.';
  assert.equal(f.status(), said); assert.equal(f.find('[data-access-draft-status]').getAttribute('role'), 'alert');
  assert.equal(f.find('[data-access-outside-wait]'), null); assert.equal(f.locked(), false); assert.equal(f.find('[name="https.origin"]').value, secure, 'what was typed is still there');
  assert.equal(f.server.exits, 0); assert.equal(f.server.ended, 0); assert.equal(f.server.runtime.blocked, false);
  assert.deepEqual(await files(f.directory), ['journal.json'], 'no policy was written');
  assert.deepEqual(f.server.records.map(row => row[0]), ['server_access_outside_started', 'server_access_outside_expired']);
  await phone.poll();
  assert.notEqual(phone.entry.error, '', 'the address is no way in again');
  // A reload still says it.
  f.state.ui = {}; await f.poll();
  assert.equal(f.status(), said); assert.equal(f.find('[data-access-draft-status]').getAttribute('role'), 'alert');
});

test('직접 확인하고 저장: the old address and a wrong code prove nothing; withdrawn, nothing changed; then the code typed at the new address does it', async t => {
  const f = await page(t, outOfReach);
  await f.typeHttps();
  const code = await f.saveOutside(), operationId = f.entry.value.pending.operationId;
  // Opened at the new address by typing it, without the link: the page asks for the code, and shows neither it nor the link.
  const phone = f.open(secure); await phone.poll(); await phone.flush();
  assert.equal(phone.find('[data-access-outside-wait] p').textContent, '이 주소가 서버로 연결돼요.\n저장한 화면에 보이는 확인 코드 6자리를 입력하세요.');
  assert.equal(phone.find('[data-access-outside-code]'), null); assert.equal(phone.find('[data-access-outside-wait] a'), null); assert.equal(phone.entry.value.outside.code, undefined);
  assert.equal(f.entry.value.outside.origins[0].confirmed, false, 'opening the page is not the proof');
  const type = async value => { phone.change('outsideCode', value); phone.find('[data-access-outside-confirm]').click(); await phone.flush(); };
  await type('12345');
  assert.equal(phone.find('[data-access-outside-error]').textContent, '확인 코드는 숫자 6자리예요.');
  await type(code === '000000' ? '000001' : '000000');
  assert.equal(phone.find('[data-access-outside-error]').textContent, '확인 코드가 달라요. 저장한 화면의 코드를 다시 보세요.');
  // The right code from the address the save was made at: that request proves nothing about the new one.
  await assert.rejects(f.api('/server-access/confirm', 'POST', { operationId, code }), error => error.code === 'server_access_confirmation_origin' && error.status === 409);
  await f.poll();
  assert.equal(f.entry.value.state, 'awaiting_confirmation'); assert.equal(f.entry.value.outside.origins[0].confirmed, false); assert.equal(f.server.exits, 0);
  // Withdrawn.
  f.find('[data-access-cancel]').click(); await f.server.access.task; await f.flush(); await f.poll();
  assert.equal(f.status(), '저장을 취소했어요. 현재 설정은 그대로예요.'); assert.equal(f.find('[data-access-outside-wait]'), null); assert.equal(f.locked(), false);
  assert.equal(f.server.exits, 0); assert.equal(f.server.ended, 0); assert.deepEqual(await files(f.directory), ['journal.json']);
  await type(code);
  // The page there learns it from that refusal: the code field goes, and what is left says the save is over.
  assert.equal(phone.find('[name="outsideCode"]'), null);
  assert.match(phone.find('[data-access-outside-over]').textContent, /^확인할 저장이 없어요\. 시간이 지났거나 취소됐어요\.\n이 주소는 이제 열리지 않아요\./, 'the address is no way in any more, and the page says that');
  await phone.poll();
  assert.notEqual(phone.entry.error, '');
  // Again, and this time the code is typed at the new address.
  f.find('[data-access-test]').click(); await f.flush();
  const second = await f.saveOutside();
  const again = f.open(secure); await again.poll(); await again.flush();
  again.change('outsideCode', second); again.find('[data-access-outside-confirm]').click(); await again.flush();
  await f.server.access.task;
  assert.equal(f.server.exits, 1);
  assert.deepEqual(f.server.records.map(row => row[0]), ['server_access_outside_started', 'server_access_outside_started', 'server_access_outside_confirmed']);
});

test('직접 확인하고 저장, the server restarted while it waited: nothing changed, the page says so and the address is closed', async t => {
  const f = await page(t, outOfReach);
  await f.typeHttps();
  await f.saveOutside();
  const phone = f.open(secure); await phone.poll();
  assert.equal(phone.entry.error, '');
  f.down = true; await f.poll();
  await f.restart(outOfReach); await f.poll();
  assert.equal(f.entry.value.state, 'failed'); assert.equal(f.entry.value.errorCode, 'server_access_interrupted');
  assert.equal(f.status(), '서버가 다시 시작되어 적용하지 않았어요. 현재 설정은 그대로예요.\n다시 테스트한 뒤 저장하세요.');
  assert.equal(f.find('[data-access-draft-status]').getAttribute('role'), 'alert'); assert.equal(f.find('[data-access-outside-wait]'), null); assert.equal(f.locked(), false);
  assert.equal(f.server.exits, 0); assert.deepEqual(await files(f.directory), ['journal.json']); assert.equal(f.entry.value.applied.https.enabled, false);
  await phone.poll();
  assert.notEqual(phone.entry.error, '');
});

// ---- what a person can read off the page (browser check of 2026-11-13) ----
// Round 2 (2026-11-13): an extra address is tried too. Out of reach from inside it is said and saved; a fault blocks.
test('an extra address is tried as well: one the server does not reach is said with what to do and can be saved; a fault on it blocks the save', async t => {
  const extraAddress = 'http://203.0.113.7:18480';
  let atExtra = { error: 'no_answer' };
  const f = await page(t, { reach: origin => origin === extraAddress ? atExtra : true });
  f.find('[data-access-extra-add="http"]').click();
  f.change('http.extra.0', extraAddress); f.find('[data-access-test]').click(); await f.flush();
  assert.deepEqual(f.server.tried, [lan, extraAddress], 'the main address and the extra one');
  assert.equal(f.testText(), '서버가 외부 접속 주소에 닿는 것을 확인했어요. 저장할 수 있어요.\n닿지 않은 추가 주소는 밖에서 열리는지 확인하세요.');
  const rows = () => f.all('[data-access-address-check]').map(node => [node.getAttribute('data-access-address-check'), node.firstChild.textContent, ...[...node.querySelectorAll('small')].map(small => small.textContent)]);
  assert.deepEqual(rows(), [
    ['passed', lan, '이 서버로 연결돼요.'],
    ['unreached', extraAddress, '서버에서는 닿지 않았어요. 밖에서 열리는지 확인하세요.', '응답이 없어요. 포트포워딩과 방화벽을 확인하세요.']]);
  assert.equal(f.find('[data-access-save]').disabled, false, 'saved without a proof: a router without NAT loopback cannot reach its own public address');
  assert.equal(f.find('[data-access-save-outside]'), null, 'and nothing to prove from outside');
  // Each reason that only looks like "out of reach from inside" is said the same way; a fault is a fault.
  for (const [answer, code, blocks] of [[{ error: 'closed' }, 'closed', false], [{ error: 'name_unknown' }, 'name_unknown', false], [{ error: 'certificate' }, 'certificate', true], [{ error: 'not_https' }, 'not_https', true], [{ answer: null }, 'other_server', true]]) {
    atExtra = answer; f.find('[data-access-test]').click(); await f.flush();
    const check = f.all('[data-access-address-check]').at(-1);
    assert.equal(check.getAttribute('data-access-address-check'), blocks ? 'failed' : 'unreached', code);
    assert.equal(f.find('[data-access-save]').disabled, blocks, code + (blocks ? ' blocks the save' : ' does not block'));
  }
  // Reached: the plain sentence.
  atExtra = true; f.find('[data-access-test]').click(); await f.flush();
  assert.equal(f.testText(), '서버가 주소에 닿는 것을 확인했어요.\n저장할 수 있어요.');
  assert.deepEqual(rows().map(row => row[0]), ['passed', 'passed']);
});

test('변경 저장 says why it cannot be pressed: a test that found a fault is not "test first"', async t => {
  const f = await page(t, { reach: origin => origin === lan ? true : { error: 'not_https' } });
  assert.equal(f.find('[data-access-save]').title, '변경한 설정으로 연결 테스트를 먼저 하세요.');
  await f.typeHttps();
  assert.equal(f.find('[data-access-save]').disabled, true);
  assert.equal(f.find('[data-access-save]').title, '검사를 통과하지 못해 저장할 수 없어요. 검사 결과를 확인하세요.');
  // Edited again: back to "test first".
  f.change('https.origin', 'https://other.example.com'); f.redraw();
  assert.equal(f.find('[data-access-save]').title, '변경한 설정으로 연결 테스트를 먼저 하세요.');
});

test('변경 저장 beside the offer to prove an address names that other button', async t => {
  const f = await page(t, outOfReach);
  await f.typeHttps();
  assert.ok(f.find('[data-access-save-outside]'));
  assert.equal(f.find('[data-access-save]').disabled, true);
  assert.equal(f.find('[data-access-save]').title, '서버가 닿지 못한 주소가 있어요. ‘직접 확인하고 저장’으로 저장하세요.');
});

test('while a save is applied the step the server reports is shown beside the buttons, and its end is said once as a toast', async t => {
  const f = await page(t);
  await f.typeHttps(); await f.save();
  const steps = () => f.all('[data-access-progress] li').map(node => node.textContent + (node.getAttribute('aria-current') === 'step' ? '*' : ''));
  assert.equal(f.find('[data-access-progress]').getAttribute('data-access-progress'), 'waiting_idle');
  assert.deepEqual(steps(), ['연결 끊기*', '서버 다시 시작', '새 주소 확인']);
  assert.ok(f.find('[data-access-progress] .loading-dot'), 'something moves');
  assert.ok(f.state.ui.serverAccess.poller, 'the page looks every second while it is applied');
  assert.deepEqual(f.notices, []);
  await f.server.access.task; await f.poll();
  assert.deepEqual(steps(), ['연결 끊기', '서버 다시 시작*', '새 주소 확인']);
  f.down = true; await f.poll();
  await f.restart(); await f.poll();
  assert.deepEqual(steps(), ['연결 끊기', '서버 다시 시작', '새 주소 확인*']);
  await f.server.access.resume(); await f.poll(); await f.flush();
  assert.equal(f.find('[data-access-progress]'), null); assert.equal(f.state.ui.serverAccess.poller, null, 'and stops looking that often');
  assert.equal(f.status(), '새 설정이 적용된 것을 확인했어요.');
  assert.ok(f.find('[data-access-draft-status]').classList.contains('is-done'), 'with a tick, not one more grey line');
  assert.deepEqual(f.notices, [{ text: '새 설정이 적용된 것을 확인했어요.', error: false }]);
  await f.poll(); await f.flush();
  assert.equal(f.notices.length, 1, 'said once');
});

test('a save that did not stay is also said as a toast that stays, once', async t => {
  const f = await page(t);
  await f.typeHttps(); await f.save();
  await f.server.access.task; f.down = true; await f.poll();
  await f.restart({ healthy: origin => origin !== secure }); await f.poll();
  await f.server.access.resume(); f.down = true; await f.poll();
  await f.restart(); await f.poll(); await f.flush();
  const said = '서버가 ' + secure + '에 닿지 못해 이전 설정으로 돌아갔어요.\n그 주소가 이 서버로 연결되는지 확인한 뒤 다시 저장하세요.';
  assert.deepEqual(f.notices, [{ text: said, error: true }]);
  await f.poll(); await f.flush(); assert.equal(f.notices.length, 1);
});

test('a page that did not make the save (opened later, or reloaded) is told for ten minutes that it was applied, and when', async t => {
  const f = await page(t);
  await f.typeHttps(); await f.save();
  await f.server.access.task; f.down = true; await f.poll();
  await f.restart(); await f.poll(); await f.server.access.resume(); await f.poll(); await f.flush();
  const ended = new Date(f.entry.value.completedAt), hhmm = String(ended.getHours()).padStart(2, '0') + ':' + String(ended.getMinutes()).padStart(2, '0');
  // The same browser after a reload: it has no save of its own to report.
  f.clock = ended.getTime() + 5000; clearInterval(f.state.ui.serverAccess.poller); f.state.ui = {}; await f.poll(); await f.flush();
  assert.equal(f.status(), hhmm + '에 저장한 설정이 적용돼 있어요.'); assert.ok(f.find('[data-access-draft-status]').classList.contains('is-done'));
  assert.equal(f.notices.length, 1, 'no second toast for a save this visit did not make');
  // Editing says "editing"; and ten minutes later it is simply the current setting.
  f.change('https.origin', 'https://vdi2.example.com'); f.redraw(); assert.equal(f.status(), '편집 중 · 아직 적용하지 않았어요.');
  f.clock = ended.getTime() + 601000; f.state.ui = {}; await f.poll();
  assert.equal(f.status(), '');
});

test('while a save waits for its proof the result panel says that, not "nothing was saved"', async t => {
  const f = await page(t, outOfReach);
  await f.typeHttps(); await f.saveOutside();
  assert.equal(f.testText(), '주소 확인을 기다리고 있어요. 확인되면 바로 적용해요.');
});

test('the address under proof after the save ended: the page there says it is over instead of still asking for the code', async t => {
  const f = await page(t, outOfReach);
  await f.typeHttps(); await f.saveOutside();
  const phone = f.open(secure); await phone.poll();
  assert.ok(phone.find('[name="outsideCode"]'));
  f.find('[data-access-cancel]').click(); await f.flush(); await f.server.access.task; await f.poll();
  // The server no longer answers at that address at all.
  await phone.poll();
  assert.equal(phone.entry.code, 'origin_not_allowed');
  assert.equal(phone.find('[name="outsideCode"]'), null);
  assert.equal(phone.find('[data-access-outside-over]').textContent, '확인할 저장이 없어요. 시간이 지났거나 취소됐어요.\n이 주소는 이제 열리지 않아요. 저장한 화면에서 다시 저장하세요.');
  assert.doesNotMatch(phone.find('.server-access-editor').textContent, /최신 상태를 확인하지 못했어요/);
});

test('포트 전달 안내 starts with the own address of the server where the install knows it', async t => {
  const f = await page(t);
  assert.equal(f.find('[name="assist.targetIp"]').value, '192.0.2.10');
  assert.match(f.find('[data-access-forward-rows]').textContent, /192\.0\.2\.10:18480/);
});
