import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { enrollmentCommand, renewalCommand, enableConsoleCommand, commandBox, createShellSetup } from '../admin/proxmox-shell-setup.js';
import { el, button, selectVisibleText } from '../admin/js/dom.js';
import { proxmox, serverInformation } from '../admin/js/system.js';

test('HTTP enrollment commands require explicit page policy and never follow a redirect', () => {
  const sha256 = 'ab'.repeat(32), origin = 'http://192.0.2.10:18400', options = { allowInsecureHttp: true, sha256 };
  assert.throws(() => enrollmentCommand(origin, { sha256 }));
  const command = enrollmentCommand(origin, options);
  assert.equal(command, `sh -c 'f=$(mktemp) || exit; curl -q -fsS "$1" -o "$f" && echo "$2  $f" | sha256sum -c --quiet - && shift 2 && python3 "$f" "$@"; s=$?; rm -f "$f"; exit $s' multitap-setup 'http://192.0.2.10:18400/setup/proxmox.py' ${sha256} --lang ko`);
  assert.equal(command.includes('-L'), false);
  assert.equal(command.includes('--location'), false);
  assert.equal(renewalCommand(origin, 'multitap-fixture@pve!broker', 'https://pve.example:8006', options), command + " --renew 'multitap-fixture@pve!broker' --endpoint 'https://pve.example:8006'");
  assert.equal(enableConsoleCommand(origin, 'multitap-fixture@pve!broker', options), command + " --enable-console 'multitap-fixture@pve!broker'");
  assert.throws(() => renewalCommand(origin, 'multitap-fixture@pve!broker', 'http://pve.example:8006', options), 'Proxmox HTTPS policy is unchanged');
  for (const value of ['http://user@192.0.2.10:18400', 'http://192.0.2.10:0', 'http://192.0.2.10:18400/path', 'http://192.0.2.10:18400?', 'http://192.0.2.10:18400\n', "http://host';id"]) assert.throws(() => enrollmentCommand(value, options));
});

test('actual HTTP Proxmox page renders the guest-agent permission renewal command', t => {
  const { document, Node } = parseHTML('<html><body></body></html>');
  const keys = ['document', 'Node', 'location'];
  const previous = Object.fromEntries(keys.map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  t.after(() => { for (const [key, value] of Object.entries(previous)) { if (value) Object.defineProperty(globalThis, key, value); else delete globalThis[key]; } });
  for (const [key, value] of Object.entries({ document, Node, location: { origin: 'http://192.0.2.10:18400', protocol: 'http:' } })) Object.defineProperty(globalThis, key, { configurable: true, value });
  const config = { configured: true, state: 'ok', endpoint: 'https://pve.example:8006', tokenId: 'multitap-fixture@pve!broker', agentPrivilegeMissing: true, vmCount: 1, setupScriptSha256: 'cd'.repeat(32) };
  const ctx = { state: { data: { proxmox: config }, pveBusy: false }, setupRequired: () => false, pageHeader: () => el('header'), syncProxmox: async () => {}, link: text => el('a', {}, text) };
  document.body.append(...proxmox(ctx));
  const commands = [...document.querySelectorAll('.shell-command code')].map(node => node.textContent);
  assert.ok(commands.includes(`sh -c 'f=$(mktemp) || exit; curl -q -fsS "$1" -o "$f" && echo "$2  $f" | sha256sum -c --quiet - && shift 2 && python3 "$f" "$@"; s=$?; rm -f "$f"; exit $s' multitap-setup 'http://192.0.2.10:18400/setup/proxmox.py' ${'cd'.repeat(32)} --lang ko --renew 'multitap-fixture@pve!broker' --endpoint 'https://pve.example:8006'`), JSON.stringify(commands));
  assert.ok(commands.every(command => !command.includes('-L')));
});

test('missing Clipboard API selects the actual visible command without hidden fields or mutation', async t => {
  const { document, Node, Event } = parseHTML('<html><body></body></html>');
  const previous = Object.fromEntries(['document', 'Node', 'navigator', 'getSelection'].map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  t.after(() => { for (const [key, value] of Object.entries(previous)) { if (value) Object.defineProperty(globalThis, key, value); else delete globalThis[key]; } });
  let selected = null;
  document.createRange = () => ({ selectNodeContents: node => { selected = node; } });
  Object.defineProperty(globalThis, 'document', { configurable: true, value: document });
  Object.defineProperty(globalThis, 'Node', { configurable: true, value: Node });
  Object.defineProperty(globalThis, 'navigator', { configurable: true, value: {} });
  Object.defineProperty(globalThis, 'getSelection', { configurable: true, value: () => ({ removeAllRanges() {}, addRange() {} }) });
  const command = enrollmentCommand('http://192.0.2.10:18400', { allowInsecureHttp: true, sha256: 'ab'.repeat(32) });
  const box = commandBox(el, button, command);
  document.body.append(box);
  box.querySelector('button').dispatchEvent(new Event('click'));
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(box.querySelector('button').textContent, '직접 선택해 복사');
  assert.equal(selected, box.querySelector('code'));
  assert.equal(selected.textContent, command);
  assert.equal(document.querySelector('textarea,input'), null);
  Object.defineProperty(globalThis, 'getSelection', { configurable: true, value: undefined });
  assert.equal(selectVisibleText(box.querySelector('code')), false, 'even selection-API absence remains a manual text fallback');
});


function serverAddressPage(t, transport, consoleOrigin = 'https://console.example') {
  const { document, Node } = parseHTML('<html><body></body></html>');
  const previous = Object.fromEntries(['document', 'Node', 'location'].map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  t.after(() => { for (const [key, value] of Object.entries(previous)) { if (value) Object.defineProperty(globalThis, key, value); else delete globalThis[key]; } });
  for (const [key, value] of Object.entries({ document, Node, location: { origin: consoleOrigin } })) Object.defineProperty(globalThis, key, { configurable: true, value });
  const ctx = {
    state: { ui: {}, data: { users: [], desktops: [], assignments: [], proxmox: { configured: false } }, serverInfo: { version: '0.5.79', deployment: { kind: 'systemd' }, update: { feedConfigured: false }, transport } },
    resource: () => ({ value: { releases: [] } }), value: () => null, hashFor: page => '#/' + page,
    pageHeader: () => el('header'), link: text => el('a', {}, text),
  };
  const render = transport => { ctx.state.serverInfo.transport = transport; document.body.replaceChildren(...serverInformation(ctx)); };
  render(transport);
  const row = label => [...document.querySelectorAll('.server-facts .facts > div')].find(node => node.querySelector('dt')?.textContent === label);
  return { document, row, render };
}

test('server settings distinguish the configured HTTP client address from the current HTTPS console', t => {
  const publicOrigin = 'http://192.0.2.10:18400';
  const { row } = serverAddressPage(t, { protocol: 'broker-transport-v1', publicOrigin, scheme: 'http', allowInsecureHttp: true, directTls: false });
  assert.equal(row('클라이언트 서버 주소').querySelector('.mono-value').textContent, publicOrigin);
  assert.match(row('클라이언트 서버 주소').textContent, /HTTP · 암호화 없음 · 포트 18400/);
  assert.equal(row('현재 관리자 페이지 주소').querySelector('.mono-value').textContent, 'https://console.example');
});

test('server settings preserve HTTPS proxy origins and avoid a duplicate address when the console matches', t => {
  const publicOrigin = 'https://broker.example';
  const { row } = serverAddressPage(t, { protocol: 'broker-transport-v1', publicOrigin, scheme: 'https', allowInsecureHttp: false, directTls: false }, publicOrigin);
  assert.equal(row('클라이언트 서버 주소').querySelector('.mono-value').textContent, publicOrigin);
  assert.match(row('클라이언트 서버 주소').textContent, /HTTPS · 암호화 연결 · 포트 443/);
  assert.equal(row('현재 관리자 페이지 주소'), undefined);
});

test('missing or contradictory transport metadata never promotes the console URL to the client address', t => {
  const valid = { protocol: 'broker-transport-v1', publicOrigin: 'http://192.0.2.10:18400', scheme: 'http', allowInsecureHttp: true, directTls: false };
  const { row, render } = serverAddressPage(t, undefined);
  for (const transport of [undefined, {}, { ...valid, scheme: 'https' }, { ...valid, allowInsecureHttp: false }, { ...valid, directTls: true }, { ...valid, publicOrigin: 'http://user@192.0.2.10:18400' }, { ...valid, publicOrigin: 'http://192.0.2.10:18400/path' }, { ...valid, publicOrigin: 'http://192.0.2.10:0' }]) {
    render(transport);
    const client = row('클라이언트 서버 주소');
    assert.equal(client.querySelector('.mono-value'), null);
    assert.ok(client.textContent.startsWith('클라이언트 서버 주소확인 필요'));
    assert.equal(row('현재 관리자 페이지 주소').querySelector('.mono-value').textContent, 'https://console.example');
  }
});

test('a console opened over HTTP says in one line that the pasted connection block travels unencrypted', t => {
  const { document, Node } = parseHTML('<html><body></body></html>');
  const previous = Object.fromEntries(['document', 'Node', 'location'].map(key => [key, Object.getOwnPropertyDescriptor(globalThis, key)]));
  t.after(() => { for (const [key, value] of Object.entries(previous)) { if (value) Object.defineProperty(globalThis, key, value); else delete globalThis[key]; } });
  for (const [key, value] of Object.entries({ document, Node, location: { origin: 'http://192.0.2.10:18400', protocol: 'http:' } })) Object.defineProperty(globalThis, key, { configurable: true, value });
  const panel = createShellSetup(el, button, async () => {}, { sha256: 'ab'.repeat(32) });
  const warnings = panel.querySelectorAll('.shell-http-warning');
  assert.equal(warnings.length, 1);
  assert.equal(warnings[0].textContent, '이 페이지는 HTTP라서, 붙여 넣는 연결 설정(비밀 토큰 포함)이 암호화되지 않은 채 서버로 가요.');
  assert.equal(panel.querySelectorAll('.shell-command code').length, 1);
  // Without the broker's hash there is no command to copy, only a plain line.
  const missing = createShellSetup(el, button, async () => {});
  assert.equal(missing.querySelector('.shell-command'), null);
  assert.equal(missing.querySelector('.import-status.error').textContent, '설치 명령을 만들지 못했어요. 페이지를 새로 고쳐 주세요.');
});
