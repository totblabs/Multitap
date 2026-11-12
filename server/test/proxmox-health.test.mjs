// pve-health-v1 (vm-config-sweep items 2 and 7): a Proxmox call refused on the
// certificate, the pinned certificate's expiry and the token's expiry reach the
// admin console as named warnings with their fix, not as generic errors.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, sign, randomBytes, X509Certificate } from 'node:crypto';
import { createServer } from 'node:https';
import { WebSocketServer } from 'ws';
import { parseHTML } from 'linkedom';
import { Proxmox } from '../src/proxmox.mjs';
import { ProxmoxHealth, pinnedCertificate, certificateFailureKind } from '../src/proxmox-health.mjs';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { proxmox as proxmoxPage } from '../admin/js/system.js';
import { el } from '../admin/js/dom.js';
import { loadAdmin, iso, workspaceFixture } from './admin-ui-harness.mjs';

// Certificates built here (no OpenSSL), with the validity each case needs.
const DAY = 86400000;
const der = (tag, ...parts) => {
  const body = Buffer.concat(parts), length = body.length < 128 ? [body.length] : body.length < 256 ? [0x81, body.length] : [0x82, body.length >> 8, body.length & 255];
  return Buffer.concat([Buffer.from([tag, ...length]), body]);
};
const seq = (...parts) => der(0x30, ...parts), oid = hex => der(0x06, Buffer.from(hex, 'hex'));
const utc = date => der(0x17, Buffer.from(date.toISOString().replace(/[-:T]/g, '').slice(2, 14) + 'Z'));
const nameOf = cn => seq(der(0x31, seq(oid('550403'), der(0x0c, Buffer.from(cn)))));
function certificate(cn, { from = -DAY, until = 30 * DAY } = {}) {
  const { privateKey, publicKey } = generateKeyPairSync('ec', { namedCurve: 'P-256' });
  const algorithm = seq(oid('2a8648ce3d040302'));
  const tbs = seq(der(0xa0, der(0x02, Buffer.from([2]))), der(0x02, randomBytes(8).fill(1, 0, 1)), algorithm, nameOf(cn),
    seq(utc(new Date(Date.now() + from)), utc(new Date(Date.now() + until))), nameOf(cn), publicKey.export({ type: 'spki', format: 'der' }));
  const body = seq(tbs, algorithm, der(0x03, Buffer.from([0]), sign('sha256', tbs, privateKey)));
  const cert = `-----BEGIN CERTIFICATE-----\n${body.toString('base64').match(/.{1,64}/g).join('\n')}\n-----END CERTIFICATE-----\n`;
  return { cert, key: privateKey.export({ type: 'pkcs8', format: 'pem' }), fingerprint: new X509Certificate(cert).fingerprint256 };
}
const node = certificate('pve.other.example'), replaced = certificate('pve.other.example');
const config = (endpoint, extra) => ({ endpoint, tokenId: 'multitap-fixture@pve!broker', secret: 'health-fixture-only-secret', ...extra });

// One Proxmox address whose certificate can be swapped, as Proxmox renewing it does.
async function fakeProxmox(t, material) {
  const server = createServer({ cert: material.cert, key: material.key }, (req, res) => { res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ data: { version: '9.0.3' } })); });
  const sockets = new WebSocketServer({ noServer: true });
  server.on('upgrade', (req, socket, head) => sockets.handleUpgrade(req, socket, head, ws => ws.close(1000)));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => { sockets.close(); server.closeAllConnections?.(); server.close(resolve); }));
  return { endpoint: `https://127.0.0.1:${server.address().port}`, present: next => server.setSecureContext({ cert: next.cert, key: next.key }) };
}

test('a certificate refusal is remembered for exactly the settings that met it and cleared by the next call that gets through', async t => {
  const fake = await fakeProxmox(t, node), adapter = new Proxmox();
  const pinned = config(fake.endpoint, { ca: node.cert, fingerprint: node.fingerprint });
  await adapter.call(pinned, '/version');
  assert.equal(adapter.health.status(pinned), null);
  // Proxmox renews the node certificate: every call stops, and the admin console hears why.
  fake.present(replaced);
  await assert.rejects(adapter.call(pinned, '/version'), error => error.code === 'proxmox_certificate_error');
  const first = adapter.health.status(pinned);
  assert.equal(first.kind, 'changed');
  assert.equal(first.since, first.lastAt);
  await assert.rejects(adapter.call(pinned, '/version'));
  assert.equal(adapter.health.status(pinned).since, first.since, 'a repeated refusal keeps when it started');
  // Other settings (a new paste) start clean; the old ones still carry it.
  const repinned = config(fake.endpoint, { ca: replaced.cert, fingerprint: replaced.fingerprint });
  assert.equal(adapter.health.status(repinned), null);
  assert.deepEqual(await adapter.call(repinned, '/version'), { version: '9.0.3' });
  assert.equal(adapter.health.status(pinned).kind, 'changed', 'a call with other settings does not clear these');
  // The old certificate comes back: the first call that gets through clears it.
  fake.present(node);
  await adapter.call(pinned, '/version');
  assert.equal(adapter.health.status(pinned), null);
  // Without a pin the certificate is not trusted at all: untrusted, not changed.
  const unpinned = config(fake.endpoint, { ca: '' });
  await assert.rejects(adapter.call(unpinned, '/version'), error => error.code === 'proxmox_certificate_error');
  assert.equal(adapter.health.status(unpinned).kind, 'untrusted');
  // An unreachable address is not a certificate matter.
  const closed = config('https://127.0.0.1:1', { ca: node.cert, fingerprint: node.fingerprint });
  await assert.rejects(adapter.call(closed, '/version'), error => error.code === 'proxmox_unreachable');
  assert.equal(adapter.health.status(closed), null);
});

test('a pinned certificate past its date is named expired', async t => {
  const old = certificate('pve.other.example', { from: -40 * DAY, until: -DAY });
  const fake = await fakeProxmox(t, old), adapter = new Proxmox();
  const pinned = config(fake.endpoint, { ca: old.cert, fingerprint: old.fingerprint });
  await assert.rejects(adapter.call(pinned, '/version'), error => error.code === 'proxmox_certificate_error');
  assert.equal(adapter.health.status(pinned).kind, 'expired');
  assert.equal(certificateFailureKind(pinned, { code: 'CERT_HAS_EXPIRED' }), 'expired');
  assert.equal(certificateFailureKind(pinned, { code: 'ERR_TLS_CERT_PIN_MISMATCH' }), 'changed');
  assert.equal(certificateFailureKind({ ...pinned, fingerprint: undefined }, { code: 'ERR_TLS_CERT_ALTNAME_INVALID' }), 'untrusted');
});

test('the console WebSocket records a refused certificate like an API call', async t => {
  const fake = await fakeProxmox(t, replaced), adapter = new Proxmox();
  adapter.call = async () => ({ port: 5901, ticket: 'PVEVNC:fixture-ticket' });
  const pinned = config(fake.endpoint, { ca: node.cert, fingerprint: node.fingerprint });
  const opened = await adapter.openConsole(pinned, { node: 'pve-a', vmid: 710 });
  await new Promise(resolve => { opened.socket.once('error', resolve); opened.socket.once('open', resolve); });
  opened.socket.terminate();
  assert.equal(adapter.health.status(pinned)?.kind, 'changed');
  fake.present(node);
  const again = await adapter.openConsole(pinned, { node: 'pve-a', vmid: 710 });
  await new Promise((resolve, reject) => { again.socket.once('open', resolve); again.socket.once('error', reject); });
  again.socket.terminate();
  assert.equal(adapter.health.status(pinned), null, 'an accepted upgrade clears it');
});

test('the pinned certificate expiry is read from the stored certificate itself', () => {
  const soon = certificate('pve.other.example', { until: 20 * DAY });
  const value = pinnedCertificate(config('https://10.0.0.5:8006', { ca: soon.cert, fingerprint: soon.fingerprint }));
  assert.equal(value.expiresAt, new Date(new X509Certificate(soon.cert).validTo).toISOString());
  assert.equal(pinnedCertificate(config('https://10.0.0.5:8006', { ca: soon.cert })), null, 'no pin: a CA is not this warning');
  assert.equal(pinnedCertificate(config('https://10.0.0.5:8006', { ca: soon.cert, fingerprint: node.fingerprint })), null);
  assert.equal(pinnedCertificate(config('https://10.0.0.5:8006', { ca: 'not a certificate', fingerprint: node.fingerprint })), null);
  assert.equal(pinnedCertificate(null), null);
  // Not a certificate error: nothing is recorded.
  const health = new ProxmoxHealth(), settings = config('https://10.0.0.5:8006', {});
  health.observe(settings, Object.assign(new Error('x'), { code: 'ECONNREFUSED' }));
  assert.equal(health.status(settings), null);
});

test('the admin API returns the refusal for the stored settings and the pinned expiry; new settings start clean', async t => {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), password = 'Health-tests-only-password-71!';
  store.addUser({ name: 'admin', displayName: 'Admin', passwordHash: await hashPassword(password), role: 'admin' });
  store.saveSetting('proxmox_initialized', '1', 'health-test-fixture');
  const health = new ProxmoxHealth();
  const adapter = { health, call: async () => ({}), inventory: async (_cfg, { targets }) => targets.map(target => ({ node: target.node, vmid: target.vmid, machineUuid: target.expectedUuid, name: 'Windows', status: 'stopped' })) };
  const origin = 'https://health-tests.example';
  const server = await buildServer({ store, vault, publicOrigin: origin, proxmox: adapter, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { await new Promise(resolve => server.close(resolve)); store.close(); });
  let cookie = '', csrf = '';
  const call = async (path, method = 'GET', data) => {
    const response = await fetch(`http://127.0.0.1:${server.address().port}/api/v1/admin` + path, { method, headers: { Cookie: cookie, ...(data ? { 'Content-Type': 'application/json' } : {}), ...(method !== 'GET' ? { Origin: origin, 'X-CSRF-Token': csrf } : {}) }, body: data ? JSON.stringify(data) : undefined });
    return { status: response.status, body: await response.json(), headers: response.headers };
  };
  const login = await call('/login', 'POST', { username: 'admin', password });
  cookie = login.headers.get('set-cookie').split(';')[0]; csrf = login.body.csrfToken;
  const soon = certificate('pve.other.example', { until: 20 * DAY });
  const saved = await call('/proxmox', 'PUT', { endpoint: 'https://10.0.0.5:8006', tokenId: 'multitap-fixture@pve!broker', secret: 'health-fixture-only-secret', ca: soon.cert, fingerprint: soon.fingerprint });
  assert.equal(saved.status, 200);
  assert.equal(saved.body.proxmox.certificateFailure, null);
  assert.equal(saved.body.proxmox.pinnedCertificate.expiresAt, new Date(new X509Certificate(soon.cert).validTo).toISOString());
  // A power, Console or status call refused on the certificate with the stored settings.
  health.observe(vault.open(store.setting('proxmox')), Object.assign(new Error('x'), { code: 'DEPTH_ZERO_SELF_SIGNED_CERT' }));
  const inventory = await call('/inventory');
  assert.equal(inventory.status, 200);
  assert.equal(inventory.body.proxmox.certificateFailure.kind, 'changed');
  assert.ok(Number.isFinite(Date.parse(inventory.body.proxmox.certificateFailure.since)));
  // The administrator pastes new settings: the old refusal no longer describes them.
  const fresh = certificate('pve.other.example', { until: 300 * DAY });
  const repasted = await call('/proxmox', 'PUT', { endpoint: 'https://10.0.0.5:8006', tokenId: 'multitap-fixture@pve!broker', secret: 'health-fixture-only-secret', ca: fresh.cert, fingerprint: fresh.fingerprint });
  assert.equal(repasted.body.proxmox.certificateFailure, null);
  assert.equal(repasted.body.proxmox.pinnedCertificate.expiresAt, new Date(new X509Certificate(fresh.cert).validTo).toISOString());
  // Without a pin there is no pinned expiry.
  const unpinned = await call('/proxmox', 'PUT', { endpoint: 'https://pve.example:8006', tokenId: 'multitap-fixture@pve!broker', secret: 'health-fixture-only-secret', ca: '' });
  assert.equal(unpinned.body.proxmox.pinnedCertificate, null);
});

test('Banner and 홈: a refused Proxmox certificate is red with its fix; a pinned one warns from 30 days, red from 7', async t => {
  const { user, inventory } = workspaceFixture();
  const base = { configured: true, state: 'ok', endpoint: 'https://10.0.0.5:8006', tokenId: 'multitap@pve!broker', hasSecret: true, ca: node.cert, fingerprint: node.fingerprint, expiresAt: iso(200 * DAY), expirySource: 'configured', consolePrivilege: 'granted' };
  inventory.proxmox = { ...base, certificateFailure: { kind: 'changed', since: iso(-60000), lastAt: iso(-1000) }, pinnedCertificate: { expiresAt: iso(20 * DAY) } };
  const f = await loadAdmin(t, 'pve-health', user, inventory);
  const banner = document.querySelector('#expiry-banner');
  const items = () => [...banner.querySelectorAll('.expiry-item')].map(node => [node.className.replace('expiry-item tone-', ''), node.querySelector('strong').textContent, node.querySelector('small').textContent]);
  assert.equal(banner.hidden, false);
  assert.deepEqual(items(), [['bad', 'Proxmox 인증서가 바뀌었어요', '전원·Console·상태 확인이 멈췄어요. 연결 설정을 다시 만들어 붙여넣으세요.']], 'the refusal, not also the expiry');
  // 홈 names it among the things to do now, instead of the general connection error.
  const todo = () => [...f.content.querySelectorAll('.todo-panel > .todo-list .todo-title')].map(node => node.textContent);
  assert.deepEqual(todo(), ['Proxmox 인증서가 바뀌었어요']);
  assert.match(f.content.querySelector('.health-line').textContent, /Proxmox 연결 오류/, 'the status line does not say connected');
  inventory.proxmox = { ...base, state: 'error', errorCode: 'proxmox_certificate_error', certificateFailure: { kind: 'expired', since: iso(-60000), lastAt: iso(-1000) } };
  document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.deepEqual(items(), [['bad', 'Proxmox 인증서가 만료됐어요', 'Proxmox에서 인증서를 갱신한 뒤 연결 설정을 다시 붙여넣으세요.']]);
  assert.deepEqual(todo(), ['Proxmox 인증서가 만료됐어요'], 'one item for one cause, not also the general connection error');
  // A pinned certificate that still works: 31 days nothing, 20 amber, 5 red, the token alongside.
  for (const [days, expected] of [[31, []], [20, [['warn', 'Proxmox 인증서 만료 20일 전', '갱신되면 연결이 끊겨요. 갱신한 뒤 연결 설정을 다시 붙여넣으세요.']]], [5, [['bad', 'Proxmox 인증서 만료 5일 전', '갱신되면 연결이 끊겨요. 갱신한 뒤 연결 설정을 다시 붙여넣으세요.']]]]) {
    inventory.proxmox = { ...base, pinnedCertificate: { expiresAt: iso(days * DAY - 60000) } };
    document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
    assert.deepEqual(items(), expected, days + ' days');
  }
  assert.ok(todo().includes('Proxmox 인증서 만료 5일 전'), '홈 lists it among the things to do now');
  inventory.proxmox = { ...base, pinnedCertificate: { expiresAt: iso(-60000) } };
  document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle();
  assert.deepEqual(items(), [['bad', 'Proxmox 인증서가 만료됐어요', 'Proxmox에서 인증서를 갱신한 뒤 연결 설정을 다시 붙여넣으세요.']]);
  // The item opens the Proxmox page.
  f.follow(banner, banner.querySelector('a.expiry-item').textContent); await f.settle();
  assert.equal(f.crumb(), 'Proxmox 연결');
});

function page(t, proxmoxState) {
  const window = parseHTML('<html><body></body></html>');
  const before = { document: globalThis.document, Node: globalThis.Node, location: globalThis.location };
  Object.assign(globalThis, { document: window.document, Node: window.Node, location: new URL('https://health-ui.example:9443/admin') });
  t.after(() => Object.assign(globalThis, before));
  const ctx = {
    state: { data: { proxmox: { configured: true, tokenId: 'multitap-fixture@pve!broker', endpoint: 'https://10.0.0.5:8006', state: 'ok', consolePrivilege: 'granted', setupScriptSha256: 'efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef', ...proxmoxState } }, user: { id: 'admin' }, csrf: 'fixture', ui: {}, pveError: '', pveErrorCode: null },
    setupRequired: () => false, syncProxmox() {}, refresh: async () => {}, notice() {}, modal() {}, api: async () => ({}),
    pageHeader: options => el('header', {}, el('h1', {}, options.title)), link: text => el('a', {}, text),
    pageAction: () => ({ current: () => true, check() {} }),
  };
  document.body.replaceChildren(...proxmoxPage(ctx));
  return document;
}

test('the Proxmox page shows a refusal met by power, Console or status even when its own last check passed', t => {
  const pinned = { ca: node.cert, fingerprint: node.fingerprint }, since = iso(-60000);
  let doc = page(t, { ...pinned, certificateFailure: { kind: 'changed', since, lastAt: since } });
  let alert = doc.querySelector('.pin-changed-alert');
  assert.equal(alert.querySelector('strong').textContent, 'Proxmox 인증서가 바뀌었어요');
  assert.match(alert.querySelector('.shell-command code').textContent, /setup\/proxmox\.py/, 'with the command that makes new settings');
  doc = page(t, { ...pinned, certificateFailure: { kind: 'expired', since, lastAt: since } });
  alert = doc.querySelector('.pin-changed-alert');
  assert.equal(alert.querySelector('strong').textContent, 'Proxmox 인증서가 만료됐어요');
  assert.equal(alert.querySelector('span').textContent, 'Proxmox에서 인증서를 갱신하세요.\nProxmox Shell에서 아래 명령을 다시 실행하고, 나온 연결 설정을 ‘연결 설정 변경’에 붙여넣으세요.');
  assert.ok(alert.querySelector('.shell-command code'));
  doc = page(t, { ca: '', certificateFailure: { kind: 'untrusted', since, lastAt: since } });
  assert.equal(doc.querySelector('.pin-changed-alert'), null);
  assert.equal(doc.querySelector('.inline-alert.is-error strong').textContent, 'Proxmox 인증서를 확인하지 못했어요');
  assert.equal(doc.querySelector('.inline-alert.is-error span').textContent, '전원·Console·상태 확인이 멈췄어요. Proxmox 연결 설정을 확인하세요.');
  // No refusal: no alert; the pinned certificate's expiry stands next to the token's.
  doc = page(t, { ...pinned, pinnedCertificate: { expiresAt: iso(20 * DAY - 60000) } });
  assert.equal(doc.querySelector('.inline-alert.is-error'), null);
  const detail = [...doc.querySelectorAll('.pve-detail')].find(node => node.querySelector('span').textContent === 'Proxmox 인증서');
  assert.equal(detail.querySelector('strong').textContent, '20일 남음');
  assert.ok(detail.classList.contains('is-warn'));
  doc = page(t, { ca: '' });
  assert.equal([...doc.querySelectorAll('.pve-detail span')].some(node => node.textContent === 'Proxmox 인증서'), false, 'no pin, no row');
});
