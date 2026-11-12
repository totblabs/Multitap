// vm-config-sweep item 5: Console on a VM whose sign-in screen may be drawn on
// a display the Console cannot show (a passed-through GPU or vGPU keeps its own
// display while the Host can only park its virtual display: signed out, locked
// or a security prompt). The page says so in one line and names 다시 시작.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { runInNewContext } from 'node:vm';
import { parseHTML } from 'linkedom';
import { Proxmox, pciPassthrough } from '../src/proxmox.mjs';
import { ConsoleRelay } from '../src/console-relay.mjs';
import { t, translatePage } from '../admin/console/i18n.js';
import { createConsoleAuthentication } from '../admin/console/console-auth.js';
import { createAccountSecurityController } from '../admin/js/account-security-controller.js';
import { createAccountSecurityView } from '../admin/js/account-security-view.js';

const uuid = '12345678-1234-1234-1234-123456789abc';
const config = { endpoint: 'https://pve.fixture.invalid:8006', tokenId: 'fixture@pve!fixture', secret: 'SERVER_ONLY_SECRET', ca: 'configured-ca-only' };

test('the Console snapshot says whether Proxmox gives the VM a PCI device', async () => {
  for (const [configuration, expected] of [[{ vga: 'std' }, false], [{ vga: 'std', hostpci0: '0000:01:00.0,mdev=nvidia-63' }, true], [{ hostpci3: 'mapping=gpu0,pcie=1' }, true], [{ hostpci0: '' }, false], [{ usb0: 'host=1234:5678' }, false]]) {
    const p = new Proxmox();
    p.call = async (_c, path) => path.startsWith('/access/') ? { '/vms/149': { 'VM.Console': 1 } } : path.endsWith('/status/current') ? { status: 'running' } : { smbios1: 'uuid=' + uuid, ...configuration };
    const value = await p.consoleSnapshot(config, 149, { node: 'node-one', expectedUuid: uuid });
    assert.equal(value.passthrough === true, expected, JSON.stringify(configuration));
    assert.equal(Object.hasOwn(value, 'passthrough'), expected, 'absent rather than false');
  }
  assert.equal(pciPassthrough(null), false);
  assert.equal(pciPassthrough({ hostpcix: '0000:01:00.0' }), false);
});

test('the hint needs a passed-through device and a Host that just said nobody is signed in and unlocked, or a security prompt is open', () => {
  const host = (capabilities, extra = {}) => ({ ws: { readyState: 1 }, capabilitiesPending: false, capabilitiesAt: Date.now(), capabilities: { service: true, ...capabilities }, ...extra });
  const relay = { hosts: new Map(), heartbeatMs: 10000 }, consoles = new ConsoleRelay(relay);
  const value = { desktopId: 'pve-149', snapshot: { passthrough: true } };
  const check = (h, snapshot = value.snapshot) => { relay.hosts.set('pve-149', h); return consoles.signinElsewhere({ ...value, snapshot }); };
  assert.equal(check(host({ interactiveSession: false })), true, 'signed out or locked');
  assert.equal(check(host({ interactiveSession: true, secureDesktop: true })), true, 'a security prompt');
  assert.equal(check(host({ interactiveSession: true })), false, 'signed in and unlocked: the worker turns the other display off');
  assert.equal(check(host({ interactiveSession: false }), {}), false, 'no PCI device in Proxmox');
  assert.equal(check(host({ interactiveSession: false }, { capabilitiesAt: Date.now() - 30001 })), false, 'old capabilities say nothing');
  assert.equal(check(host({ interactiveSession: false }, { capabilitiesPending: true })), false);
  assert.equal(check(host({ interactiveSession: false }, { ws: { readyState: 3 } })), false, 'Host gone');
  assert.equal(check(host({ interactiveSession: false, service: false })), false);
  relay.hosts.delete('pve-149');
  assert.equal(consoles.signinElsewhere(value), false, 'no Host');
  consoles.wss.close();
});

// The shipped page, run in a DOM with noVNC and the socket replaced.
const html = await readFile(new URL('../admin/console/index.html', import.meta.url), 'utf8');
const script = (await readFile(new URL('../admin/console/console.js', import.meta.url), 'utf8')).replace(/^import .+;\r?\n/gm, '').replace(/\bboot\(\);\s*$/, '')
  .replace(/import\('\/admin\/js\/account-security-controller\.js'\),import\('\/admin\/js\/account-security-view\.js'\),/, '{createAccountSecurityController},{createAccountSecurityView},');
const tick = () => new Promise(resolve => setImmediate(resolve));
function consolePage(grant) {
  const { document, window: dom } = parseHTML(html), window = new EventTarget(), responses = [{ status: 200, body: { csrfToken: 'S'.repeat(43), user: { username: 'fixture-user' } } }, { status: 200, body: grant }];
  const timers = new Map(), instances = []; let sequence = 0;
  for (const node of document.querySelectorAll('dialog')) { node.open = false; node.showModal = () => { node.open = true; }; node.close = () => { node.open = false; }; }
  const fetch = async () => { const next = responses.shift(); assert.ok(next, 'unexpected request'); return { status: next.status, ok: true, json: async () => next.body }; };
  class RFB extends EventTarget { constructor() { super(); instances.push(this); } disconnect() {} focus() {} blur() {} getImageData() { return { data: [] }; } }
  class WebSocket extends EventTarget { close() {} }
  const context = { createAccountSecurityController, createAccountSecurityView, document, window, location: { pathname: '/console/pve-170', origin: 'https://fixture.invalid', protocol: 'https:', hostname: 'fixture.invalid' }, navigator: { userAgent: 'Fixture browser' },
    matchMedia: () => ({ matches: false, addEventListener() {} }), fetch, AbortSignal, URL, RFB, WebSocket, initLogging() {}, releaseCapture() {}, createHangulKeyAdapter: () => ({ reset() {} }), t, translatePage,
    setTimeout: fn => { const id = ++sequence; timers.set(id, fn); return id; }, clearTimeout: id => timers.delete(id),
    createConsoleAuthentication: options => createConsoleAuthentication({ ...options, schedule: fn => { const id = ++sequence; timers.set(id, fn); return id; }, cancel: id => timers.delete(id) }) };
  runInNewContext(script, context);
  const $ = id => document.getElementById(id);
  return { $, instances, dom, async open() { $('username').value = 'fixture-user'; $('password').value = 'synthetic-password'; $('login-form').dispatchEvent(new dom.Event('submit', { cancelable: true })); for (let i = 0; i < 6; i++) await tick(); assert.equal(instances.length, 1, 'connected'); instances[0].dispatchEvent(new Event('connect')); } };
}
const grant = extra => ({ id: '0d4b3f0e-6a52-4a7e-9a39-1f3c55d1a0b2', wsPath: '/console/ws/0d4b3f0e-6a52-4a7e-9a39-1f3c55d1a0b2', expiresAt: new Date(Date.now() + 30000).toISOString(), desktop: { id: 'pve-170', name: '업무 VM' },
  clipboardPolicy: { textToRemote: false, textToLocal: false }, qemuConsolePreparation: { status: 'isolated' }, ...extra });

test('the Console page shows the one line while connected when the server sends the hint, and nothing otherwise', async () => {
  const page = consolePage(grant({ consoleHint: 'signin_other_display' }));
  await page.open();
  assert.equal(page.$('frame-note').hidden, false);
  assert.equal(page.$('frame-note').textContent, '로그인 화면이 콘솔에 보이지 않는 다른 화면에 있을 수 있어요. 데스크톱을 다시 시작하면 자동으로 로그인돼요.');
  const plain = consolePage(grant({}));
  await plain.open();
  assert.equal(plain.$('frame-note').hidden, true);
  const other = consolePage(grant({ consoleHint: 'something_else' }));
  await other.open();
  assert.equal(other.$('frame-note').hidden, true, 'only the one known hint');
  assert.equal(t('console.console.101').includes('\n'), false, 'one line');
});
