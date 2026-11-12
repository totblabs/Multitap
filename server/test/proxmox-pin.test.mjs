import { test } from 'node:test';
import assert from 'node:assert/strict';
import { generateKeyPairSync, sign, randomBytes, X509Certificate } from 'node:crypto';
import { createServer } from 'node:https';
import { WebSocket, WebSocketServer } from 'ws';
import { parseHTML } from 'linkedom';
import { Proxmox, validateProxmox, proxmoxTls, validFingerprint } from '../src/proxmox.mjs';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { hashPassword } from '../src/security.mjs';
import { decodeConnectionBlock } from '../admin/proxmox-shell-setup.js';
import { proxmox as proxmoxPage } from '../admin/js/system.js';
import { el } from '../admin/js/dom.js';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

// A Proxmox node certificate that does not carry the address the broker uses
// (an IP added later, a replaced certificate): the setup script pins it, and
// every connection then accepts exactly that certificate and nothing else.
// The certificates are built here, so the test needs no OpenSSL.
const DAY = 86400000;
const der = (tag, ...parts) => {
  const body = Buffer.concat(parts), length = body.length < 128 ? [body.length] : body.length < 256 ? [0x81, body.length] : [0x82, body.length >> 8, body.length & 255];
  return Buffer.concat([Buffer.from([tag, ...length]), body]);
};
const seq = (...parts) => der(0x30, ...parts), oid = hex => der(0x06, Buffer.from(hex, 'hex'));
const utc = date => der(0x17, Buffer.from(date.toISOString().replace(/[-:T]/g, '').slice(2, 14) + 'Z'));
const nameOf = cn => seq(der(0x31, seq(oid('550403'), der(0x0c, Buffer.from(cn)))));
function certificate(cn, issuer = null) {
  const { privateKey, publicKey } = generateKeyPairSync('ec', { namedCurve: 'P-256' });
  const algorithm = seq(oid('2a8648ce3d040302'));
  const tbs = seq(der(0xa0, der(0x02, Buffer.from([2]))), der(0x02, randomBytes(8).fill(1, 0, 1)), algorithm, issuer ? nameOf(issuer.cn) : nameOf(cn),
    seq(utc(new Date(Date.now() - DAY)), utc(new Date(Date.now() + 30 * DAY))), nameOf(cn), publicKey.export({ type: 'spki', format: 'der' }));
  const body = seq(tbs, algorithm, der(0x03, Buffer.from([0]), sign('sha256', tbs, issuer ? issuer.privateKey : privateKey)));
  const cert = `-----BEGIN CERTIFICATE-----\n${body.toString('base64').match(/.{1,64}/g).join('\n')}\n-----END CERTIFICATE-----\n`;
  return { cn, cert, privateKey, key: privateKey.export({ type: 'pkcs8', format: 'pem' }), fingerprint: new X509Certificate(cert).fingerprint256 };
}
const node = certificate('pve.other.example'), replaced = certificate('pve.other.example');
const issuer = certificate('Fixture Issuing CA'), issued = certificate('pve.issued.example', issuer);

async function fakeProxmox(t, material) {
  const requests = [];
  const server = createServer({ cert: material.cert, key: material.key }, (req, res) => {
    requests.push({ url: req.url, auth: req.headers.authorization });
    if (req.url === '/api2/json/nodes/pve-a/qemu/710/vncproxy' && req.method === 'POST') { res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ data: { port: 5901, ticket: 'PVEVNC:fixture-ticket' } })); return; }
    res.writeHead(200, { 'Content-Type': 'application/json' }); res.end(JSON.stringify({ data: { version: '9.0.3' } }));
  });
  const sockets = new WebSocketServer({ noServer: true }), upgrades = [];
  server.on('upgrade', (req, socket, head) => { upgrades.push(req.url); sockets.handleUpgrade(req, socket, head, ws => ws.close(1000)); });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(() => new Promise(resolve => { sockets.close(); server.closeAllConnections?.(); server.close(resolve); }));
  return { endpoint: `https://127.0.0.1:${server.address().port}`, requests, upgrades };
}
const config = (endpoint, extra) => ({ endpoint, tokenId: 'multitap-fixture@pve!broker', secret: 'pin-fixture-only-secret', ...extra });
const certificateError = error => error?.code === 'proxmox_certificate_error';

test('a pinned certificate connects by fingerprint whatever its names, and nothing else connects', async t => {
  const fake = await fakeProxmox(t, node), adapter = new Proxmox();
  // The name does not match 127.0.0.1: CA trust alone is refused, as before.
  await assert.rejects(adapter.call(config(fake.endpoint, { ca: node.cert }), '/version'), certificateError);
  await assert.rejects(adapter.call(config(fake.endpoint, { ca: '' }), '/version'), certificateError);
  assert.deepEqual(await adapter.call(config(fake.endpoint, { ca: node.cert, fingerprint: node.fingerprint }), '/version'), { version: '9.0.3' });
  assert.equal(fake.requests.length, 1);
  assert.equal(fake.requests[0].auth, 'PVEAPIToken=multitap-fixture@pve!broker=pin-fixture-only-secret');
  // Another fingerprint, or the pin with another trust anchor: refused before any request (the token never leaves).
  await assert.rejects(adapter.call(config(fake.endpoint, { ca: node.cert, fingerprint: replaced.fingerprint }), '/version'), certificateError);
  await assert.rejects(adapter.call(config(fake.endpoint, { ca: replaced.cert, fingerprint: node.fingerprint }), '/version'), certificateError);
  await assert.rejects(adapter.call(config(fake.endpoint, { ca: '', fingerprint: node.fingerprint }), '/version'), certificateError, 'a pin without its certificate never connects');
  await assert.rejects(adapter.call(config(fake.endpoint, { ca: node.cert, fingerprint: node.fingerprint.toLowerCase() }), '/version'), certificateError, 'a malformed stored pin never connects');
  assert.equal(fake.requests.length, 1);
});

test('Proxmox replacing the pinned certificate stops the connection with the certificate error', async t => {
  const before = await fakeProxmox(t, node), after = await fakeProxmox(t, replaced), adapter = new Proxmox();
  const pinned = { ca: node.cert, fingerprint: node.fingerprint };
  await adapter.call(config(before.endpoint, pinned), '/version');
  await assert.rejects(adapter.call(config(after.endpoint, pinned), '/version'), certificateError);
  assert.equal(after.requests.length, 0);
});

test('a CA-issued node certificate (custom or ACME) is pinned as its own trust anchor', async t => {
  const fake = await fakeProxmox(t, issued), adapter = new Proxmox();
  await assert.rejects(adapter.call(config(fake.endpoint, { ca: issued.cert }), '/version'), certificateError, 'without the pin the name and the issuer still count');
  assert.deepEqual(await adapter.call(config(fake.endpoint, { ca: issued.cert, fingerprint: issued.fingerprint }), '/version'), { version: '9.0.3' });
  // The issuer itself is not trusted through the pin: another certificate it issued is refused.
  const sibling = certificate('pve.issued.example', issuer), other = await fakeProxmox(t, sibling);
  await assert.rejects(adapter.call(config(other.endpoint, { ca: issued.cert, fingerprint: issued.fingerprint }), '/version'), certificateError);
  assert.equal(other.requests.length, 0);
});

test('the console WebSocket uses the same pin', async t => {
  const fake = await fakeProxmox(t, node), adapter = new Proxmox();
  const pinned = config(fake.endpoint, { ca: node.cert, fingerprint: node.fingerprint });
  const opened = await adapter.openConsole(pinned, { node: 'pve-a', vmid: 710 });
  await new Promise((resolve, reject) => { opened.socket.once('open', resolve); opened.socket.once('error', reject); });
  opened.socket.terminate();
  assert.equal(fake.upgrades.length, 1);
  assert.match(fake.upgrades[0], /^\/api2\/json\/nodes\/pve-a\/qemu\/710\/vncwebsocket\?port=5901&vncticket=/);
  // The same options with another pin: the handshake fails and the upgrade is never sent.
  for (const options of [proxmoxTls({ ...pinned, fingerprint: replaced.fingerprint }), proxmoxTls({ ...pinned, fingerprint: undefined })]) {
    const socket = new WebSocket(fake.endpoint.replace('https:', 'wss:') + '/api2/json/nodes/pve-a/qemu/710/vncwebsocket', ['binary'], { ...options, handshakeTimeout: 4000 });
    const error = await new Promise(resolve => { socket.once('error', resolve); socket.once('open', () => resolve(null)); });
    assert.ok(error, 'refused'); socket.terminate();
  }
  assert.equal(fake.upgrades.length, 1);
});

test('stored settings accept a pin only with its certificate, in Proxmox form, and keep or clear it on purpose', () => {
  const base = { endpoint: 'https://10.0.0.5:8006', tokenId: 'multitap-fixture@pve!broker', secret: 'pin-fixture-only-secret' };
  assert.equal(validFingerprint(node.fingerprint), true);
  const saved = validateProxmox({ ...base, ca: node.cert, fingerprint: node.fingerprint });
  assert.equal(saved.fingerprint, node.fingerprint); assert.equal(saved.ca, node.cert);
  assert.equal('fingerprint' in validateProxmox({ ...base, ca: node.cert }), false, 'no pin, no field');
  for (const fingerprint of [node.fingerprint.toLowerCase(), node.fingerprint.replaceAll(':', ''), node.fingerprint.slice(3), node.fingerprint + ':00', 'SHA256:' + node.fingerprint, 7]) {
    assert.throws(() => validateProxmox({ ...base, ca: node.cert, fingerprint }), error => error.code === 'invalid_proxmox_fingerprint', String(fingerprint));
  }
  assert.throws(() => validateProxmox({ ...base, ca: '', fingerprint: node.fingerprint }), error => error.code === 'invalid_proxmox_fingerprint', 'a pin needs its certificate');
  assert.throws(() => validateProxmox({ ...base, ca: replaced.cert, fingerprint: node.fingerprint }), error => error.code === 'invalid_proxmox_fingerprint', 'the pin is the certificate given');
  // A renewal or an edit without the field keeps it; an empty field or a new certificate drops it.
  assert.equal(validateProxmox({ endpoint: base.endpoint, tokenId: base.tokenId }, saved).fingerprint, node.fingerprint);
  assert.equal(validateProxmox({ endpoint: base.endpoint, tokenId: base.tokenId, ca: node.cert, expiresAt: '2099-01-01T00:00:00Z' }, saved).fingerprint, node.fingerprint);
  assert.equal('fingerprint' in validateProxmox({ endpoint: base.endpoint, tokenId: base.tokenId, fingerprint: '' }, saved), false);
  assert.equal('fingerprint' in validateProxmox({ endpoint: base.endpoint, tokenId: base.tokenId, ca: replaced.cert }, saved), false);
  assert.equal('fingerprint' in validateProxmox({ endpoint: 'https://pve.example.com:8006', tokenId: base.tokenId, secret: base.secret }, saved), false, 'another address starts without the pin');
});

test('the pasted connection block carries the pin only together with its certificate', () => {
  const block = data => 'MULTITAP-CONNECTION-V1:\n' + Buffer.from(JSON.stringify(data)).toString('base64').match(/.{1,76}/g).join('\n') + '\nEND-MULTITAP-CONNECTION';
  const value = { endpoint: 'https://10.0.0.5:8006', tokenId: 'multitap-fixture@pve!broker', secret: 'pin-fixture-only-secret', ca: node.cert, fingerprint: node.fingerprint, expiresAt: '2099-01-01T00:00:00Z' };
  assert.deepEqual(decodeConnectionBlock(block(value)), { ...value, expiresAt: '2099-01-01T00:00:00.000Z' });
  const { expiresAt, ...withoutExpiry } = value;
  assert.equal(decodeConnectionBlock(block(withoutExpiry)).fingerprint, node.fingerprint);
  for (const broken of [{ ...value, fingerprint: node.fingerprint.toLowerCase() }, { ...value, fingerprint: '' }, { ...value, ca: '' }, { ...value, fingerprint: 1 }, { ...value, pin: node.fingerprint }]) {
    assert.throws(() => decodeConnectionBlock(block(broken)), /연결 설정 형식이 올바르지 않아요/);
  }
});

test('the admin API stores the pin sealed, returns it, and a refused certificate on every registered VM is a connection error', async t => {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), password = 'Pin-tests-only-password-71!';
  store.addUser({ name: 'admin', displayName: 'Admin', passwordHash: await hashPassword(password), role: 'admin' });
  store.saveSetting('proxmox_initialized', '1', 'pin-test-fixture');
  let refuse = false;
  const adapter = {
    call: async () => { if (refuse) throw Object.assign(new Error('x'), { code: 'proxmox_certificate_error' }); return {}; },
    inventory: async (_cfg, { targets }) => targets.map(target => refuse ? { ...target, status: 'unknown', powerError: 'proxmox_certificate_error' } : { node: target.node, vmid: target.vmid, machineUuid: target.expectedUuid, name: 'Windows', status: 'stopped' }),
  };
  const origin = 'https://pin-tests.example';
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
  const input = { endpoint: 'https://10.0.0.5:8006', tokenId: 'multitap-fixture@pve!broker', secret: 'pin-fixture-only-secret', ca: node.cert, fingerprint: node.fingerprint };
  assert.equal((await call('/proxmox', 'PUT', { ...input, fingerprint: 'AB' })).body.error.code, 'invalid_proxmox_fingerprint');
  const saved = await call('/proxmox', 'PUT', input);
  assert.equal(saved.status, 200); assert.equal(saved.body.proxmox.fingerprint, node.fingerprint);
  assert.equal(vault.open(store.setting('proxmox')).fingerprint, node.fingerprint);
  assert.equal(store.setting('proxmox').includes(node.fingerprint), false, 'sealed with the rest of the connection');
  store.syncProxmox([{ vmid: 710, node: 'pve-a', name: 'Windows', status: 'stopped' }, { vmid: 711, node: 'pve-a', name: 'Windows 2', status: 'stopped' }], 'pin-test-seed');
  enrollFixtureHosts(store, ['pve-710', 'pve-711']);
  const ok = await call('/proxmox/sync', 'POST', {});
  assert.equal(ok.status, 200); assert.equal(ok.body.proxmox.state, 'ok');
  refuse = true;
  const refused = await call('/proxmox/sync', 'POST', {});
  assert.equal(refused.status, 200);
  assert.deepEqual([refused.body.proxmox.state, refused.body.proxmox.errorCode, refused.body.unavailable], ['error', 'proxmox_certificate_error', 2]);
  assert.equal(refused.body.onboarding.proxmoxRequired, false, 'an installation that worked stays set up');
});

function page(t, proxmoxState, { pveError = '', pveErrorCode = null } = {}) {
  const window = parseHTML('<html><body></body></html>');
  const before = { document: globalThis.document, Node: globalThis.Node, location: globalThis.location };
  Object.assign(globalThis, { document: window.document, Node: window.Node, location: new URL('https://pin-ui.example:9443/admin') });
  t.after(() => Object.assign(globalThis, before));
  const ctx = {
    state: { data: { proxmox: { configured: true, tokenId: 'multitap-fixture@pve!broker', endpoint: 'https://10.0.0.5:8006', state: 'ok', consolePrivilege: 'granted', setupScriptSha256: 'efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef', ...proxmoxState } }, user: { id: 'admin' }, csrf: 'fixture', ui: {}, pveError, pveErrorCode },
    setupRequired: () => false, syncProxmox() {}, refresh: async () => {}, notice() {}, modal() {}, api: async () => ({}),
    pageHeader: options => el('header', {}, el('h1', {}, options.title)), link: text => el('a', {}, text),
    pageAction: () => ({ current: () => true, check() {} }),
  };
  document.body.replaceChildren(...proxmoxPage(ctx));
  return document;
}

test('when the pinned certificate no longer matches, the Proxmox page says so and gives the command that makes a new setting', t => {
  const pinned = { ca: node.cert, fingerprint: node.fingerprint };
  for (const [state, options] of [[{ ...pinned, state: 'error', errorCode: 'proxmox_certificate_error' }, {}], [pinned, { pveError: '서버 인증서를 검증하지 못했어요.', pveErrorCode: 'proxmox_certificate_error' }]]) {
    const doc = page(t, state, options), alert = doc.querySelector('.pin-changed-alert');
    assert.ok(alert);
    assert.equal(alert.querySelector('strong').textContent, 'Proxmox 인증서가 바뀌었어요');
    assert.equal(alert.querySelector('span').textContent, '저장된 인증서와 지금 Proxmox 인증서가 달라서 연결하지 않았어요.\nProxmox Shell에서 아래 명령을 다시 실행하고, 나온 연결 설정을 ‘연결 설정 변경’에 붙여넣으세요.');
    assert.equal(alert.querySelector('.shell-command code').textContent, "sh -c 'f=$(mktemp) || exit; curl -q -fsS \"$1\" -o \"$f\" && echo \"$2  $f\" | sha256sum -c --quiet - && shift 2 && python3 \"$f\" \"$@\"; s=$?; rm -f \"$f\"; exit $s' multitap-setup 'https://pin-ui.example:9443/setup/proxmox.py' efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef --lang ko");
    assert.ok([...alert.querySelectorAll('button')].some(button => button.textContent === '다시 확인'));
  }
  // Without a pin, or another error with a pin: the usual line.
  for (const [state, options] of [[{ ca: node.cert, state: 'error', errorCode: 'proxmox_certificate_error' }, {}], [{ ...pinned, state: 'error', errorCode: 'proxmox_unreachable' }, {}]]) {
    const doc = page(t, state, options);
    assert.equal(doc.querySelector('.pin-changed-alert'), null);
    assert.equal(doc.querySelector('.inline-alert.is-error strong').textContent, '연결 확인 실패');
  }
  // The direct-entry form shows the pin next to the certificate it belongs to.
  const doc = page(t, pinned), field = doc.querySelector('.pve-raw-form input[name=fingerprint]');
  assert.equal(field.getAttribute('value'), node.fingerprint);
  assert.ok(doc.querySelector('.pve-raw-form textarea[name=ca]'));
});
