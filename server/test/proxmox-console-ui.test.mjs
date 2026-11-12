import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parseHTML } from 'linkedom';
import { proxmox } from '../admin/js/system.js';
import { createShellSetup } from '../admin/proxmox-shell-setup.js';
import { el, button } from '../admin/js/dom.js';

// Proxmox 연결: the setup command grants everything the
// product uses in one go, so the page has no separate permission buttons. An
// installation that lacks VM.Console sees one inline line with its one
// command; the sound card is not Multitap's. Standalone page DOM; the
// owner/operation fencing is exercised in admin-page-workflow.test.mjs.
// What the broker reports as the SHA-256 of its served setup script.
const SHA256 = 'efefefefefefefefefefefefefefefefefefefefefefefefefefefefefefefef';
const SETUP = "sh -c 'f=$(mktemp) || exit; curl -q -fsS \"$1\" -o \"$f\" && echo \"$2  $f\" | sha256sum -c --quiet - && shift 2 && python3 \"$f\" \"$@\"; s=$?; rm -f \"$f\"; exit $s' multitap-setup";
function fixture(t, config = {}, { granted = false } = {}) {
  const window = parseHTML('<html><body></body></html>');
  const before = { document: globalThis.document, Node: globalThis.Node, location: globalThis.location };
  Object.assign(globalThis, { document: window.document, Node: window.Node, location: new URL('https://console-ui.example:9443/admin') });
  t.after(() => Object.assign(globalThis, before));
  const requests = [], notices = [], dialogs = [], refreshed = [], actions = new Map();
  const ctx = {
    state: { data: { proxmox: { configured: true, tokenId: 'multitap-fixture@pve!broker', endpoint: 'https://pve.example:8006', state: 'ok', consolePrivilege: 'granted', setupScriptSha256: SHA256, ...config } }, user: { id: 'admin' }, csrf: 'fixture', ui: {} },
    setupRequired: () => false, syncProxmox() {}, refresh: async () => { refreshed.push(true); },
    render: () => document.body.replaceChildren(...proxmox(ctx)),
    pageHeader: options => el('header', {}, el('h1', {}, options.title), el('div', { class: 'page-actions' }, options.actions)),
    link: text => el('a', {}, text), notice: (...args) => notices.push(args),
    modal: (...args) => dialogs.push(args), api: async (...args) => { requests.push(args); return { granted }; },
    pageAction: key => {
      const ui = ctx.state.ui, actor = ctx.state.user, csrf = ctx.state.csrf, token = {};
      actions.set(key, token);
      const current = () => ctx.state.ui === ui && ctx.state.user === actor && ctx.state.csrf === csrf && actions.get(key) === token;
      return { current, check: () => { if (!current()) throw Object.assign(Error('fixture owner changed'), { code: 'admin_session_changed' }); } };
    },
  };
  ctx.render();
  const settle = async () => { for (let i = 0; i < 5; i++) await new Promise(resolve => setImmediate(resolve)); };
  const tile = label => [...document.querySelectorAll('.pve-detail')].find(node => node.querySelector('span').textContent === label);
  const click = label => { const node = [...document.querySelectorAll('button')].find(node => node.textContent === label); assert.ok(node, label); node.dispatchEvent(new window.Event('click')); };
  return { window, ctx, requests, notices, dialogs, refreshed, settle, tile, click, alert: () => document.querySelector('.console-permission-alert') };
}
const headerButtons = () => [...document.querySelectorAll('header .page-actions button')].map(node => node.textContent);

test('a complete installation: no permission buttons, no warning, the console permission shown as present, nothing about a sound card', t => {
  const f = fixture(t);
  assert.deepEqual(headerButtons(), ['새로고침', '토큰 갱신'], 'the separate 긴급 콘솔 권한 and 사운드카드 권한 buttons are gone');
  assert.equal(f.alert(), null);
  assert.deepEqual([...document.querySelectorAll('.pve-detail span')].map(node => node.textContent), ['연결 계정', '마지막 확인', '토큰 유효기간', '긴급 콘솔 권한']);
  assert.deepEqual([f.tile('긴급 콘솔 권한').querySelector('strong').textContent, f.tile('긴급 콘솔 권한').querySelector('small').textContent, f.tile('긴급 콘솔 권한').classList.contains('is-warn')], ['있음', 'Proxmox 권한 VM.Console', false]);
  assert.doesNotMatch(document.body.textContent, /사운드카드|HWType|sound/i);
  assert.equal(f.requests.length, 0); assert.equal(f.dialogs.length, 0);
  // Not asked yet (a restarted server before its first sync): said as that, not as missing.
  const unknown = fixture(t, { consolePrivilege: 'unknown' });
  assert.equal(unknown.alert(), null);
  assert.deepEqual([unknown.tile('긴급 콘솔 권한').querySelector('strong').textContent, unknown.tile('긴급 콘솔 권한').querySelector('small').textContent], ['확인 필요', '새로고침하면 확인해요']);
  // An answer from a server that does not report it is not read as missing either.
  assert.equal(fixture(t, { consolePrivilege: undefined }).alert(), null);
});

test('an installation without VM.Console: one inline line where the account is shown, one copyable command, one check; still missing stays inline', async t => {
  const f = fixture(t, { consolePrivilege: 'missing' });
  assert.deepEqual(headerButtons(), ['새로고침', '토큰 갱신'], 'no header button: the line carries the fix');
  const alert = f.alert();
  assert.ok(alert.closest('.pve-connection-card'), 'inside the card of the connection it talks about');
  assert.equal(alert.getAttribute('role'), 'status'); assert.equal(alert.querySelector('strong').textContent, '긴급 콘솔 권한(VM.Console)이 없어요');
  assert.equal(alert.querySelector('span').textContent, '사용자가 Console(긴급 콘솔)을 열 수 없어요.\nProxmox Shell(root)에서 아래 명령을 실행한 뒤 ‘권한 확인’을 누르세요.');
  assert.deepEqual([...alert.querySelectorAll('.shell-command code')].map(node => node.textContent), [SETUP + " 'https://console-ui.example:9443/setup/proxmox.py' " + SHA256 + " --lang ko --enable-console 'multitap-fixture@pve!broker'"]);
  assert.equal(alert.querySelector('.shell-command code').textContent.includes('--renew'), false);
  assert.deepEqual([f.tile('긴급 콘솔 권한').querySelector('strong').textContent, f.tile('긴급 콘솔 권한').querySelector('small').textContent, f.tile('긴급 콘솔 권한').classList.contains('is-warn')], ['없음', '사용자가 Console을 열 수 없어요', true]);
  assert.equal(f.requests.length, 0, 'showing the line asks nothing');
  f.click('권한 확인'); await f.settle();
  assert.deepEqual(f.requests, [['/proxmox/console-permission', 'POST', {}]]);
  assert.equal(f.refreshed.length, 1, 'the page reloads the server state after the check');
  assert.deepEqual(f.notices, [], 'what is still missing is not a passing message');
  assert.equal(f.alert().querySelector('small.is-error').textContent, '아직 권한이 없어요. 명령을 끝낸 뒤 다시 확인하세요.'); assert.equal(f.alert().querySelector('small.is-error').getAttribute('role'), 'alert');
  assert.equal(f.dialogs.length, 0, 'no dialog in between');
});

test('the check succeeds: a passing confirmation, and the line goes with the next state', async t => {
  const f = fixture(t, { consolePrivilege: 'missing' }, { granted: true });
  f.ctx.refresh = async () => { f.refreshed.push(true); f.ctx.state.data.proxmox.consolePrivilege = 'granted'; };
  f.click('권한 확인'); await f.settle();
  assert.deepEqual(f.notices, [['긴급 콘솔 권한을 확인했어요.']]);
  assert.equal(f.alert(), null); assert.equal(f.tile('긴급 콘솔 권한').querySelector('strong').textContent, '있음');
  // A failed request (the server or Proxmox did not answer) is a passing error message, not a claim about the permission.
  const failing = fixture(t, { consolePrivilege: 'missing' });
  failing.ctx.api = async () => { throw new Error('Proxmox에 연결하지 못했어요.'); };
  failing.click('권한 확인'); await failing.settle();
  assert.deepEqual(failing.notices, [['Proxmox에 연결하지 못했어요.', true]]); assert.equal(failing.alert().querySelector('small.is-error'), null); assert.equal(failing.ctx.state.ui.consoleChecking, false);
});

test('an account made by hand is told truthfully who must add it', t => {
  for (const tokenId of ['root@pam!token', 'vdi-api@pve!multitap', "multitap-x@pve!broker';id"]) {
    const manual = fixture(t, { consolePrivilege: 'missing', tokenId });
    assert.equal(manual.alert().querySelector('.shell-command'), null, tokenId + ': no command is offered for an account the script cannot prove it made');
    assert.equal(manual.alert().querySelector('span').textContent, '사용자가 Console(긴급 콘솔)을 열 수 없어요.\nMultitap 명령으로 만든 전용 계정이 아니라서 명령을 드릴 수 없어요.\nProxmox 관리자가 이 계정의 /vms 권한에 VM.Console을 추가한 뒤 ‘권한 확인’을 누르세요.');
    assert.ok([...manual.alert().querySelectorAll('button')].some(node => node.textContent === '권한 확인'), 'the check works for any account');
    assert.deepEqual(manual.notices, [], 'no floating message: the line is where the account is');
  }
});

test('a new installation: the one command of the first connection says it grants the console permission too, and no second step exists', async t => {
  const window = parseHTML('<html><body></body></html>');
  const before = { document: globalThis.document, Node: globalThis.Node, location: globalThis.location };
  Object.assign(globalThis, { document: window.document, Node: window.Node, location: new URL('https://console-ui.example:9443/admin') });
  t.after(() => Object.assign(globalThis, before));
  const panel = createShellSetup(el, button, async () => {}, { sha256: SHA256 });
  assert.deepEqual([...panel.querySelectorAll('.shell-command code')].map(node => node.textContent), [SETUP + " 'https://console-ui.example:9443/setup/proxmox.py' " + SHA256 + ' --lang ko']);
  assert.equal(panel.querySelector('.shell-http-warning'), null, 'an HTTPS console has no plain-HTTP line');
  assert.match(panel.textContent, /VM 조회·전원·긴급 콘솔 권한을 한 번에 만들어요\./);
  assert.doesNotMatch(panel.textContent, /사운드카드|HWType/);
  // The script that command runs creates the role with exactly these privileges (the Python tests run it; this reads it).
  const script = await readFile(new URL('../setup/proxmox.py', import.meta.url), 'utf8');
  assert.match(script, /return 'VM\.Audit VM\.PowerMgmt VM\.Console' \+ \(' ' \+ privilege if privilege else ''\)/);
  assert.equal(script.includes('--enable-sound-card'), false); assert.equal(/def enable_sound_card/.test(script), false);
  assert.equal(script.includes("'--privs', 'VM.Console'"), true, 'an older installation can still add it with --enable-console');
  assert.match(script, /OWN_USER = r'multitap-/);
  // No console source offers a sound-card permission or an "add a sound card" action.
  // Their words live in the text catalogs (admin/i18n).
  for (const file of ['js/system.js', 'js/overview.js', 'js/hosts.js', 'proxmox-shell-setup.js', 'i18n/ko.js', 'i18n/en.js']) {
    const source = await readFile(new URL('../admin/' + file, import.meta.url), 'utf8');
    assert.doesNotMatch(source, /HWType|사운드카드 권한|사운드카드를 추가|sound-card-permission/, file);
  }
  assert.match(await readFile(new URL('../admin/i18n/ko.js', import.meta.url), 'utf8'), /Proxmox에서 VM에 오디오 장치를 추가하세요 \(Hardware → Add → Audio Device\)\./);
});

test('notices have one place: a toast at the bottom, never in the page flow under the top bar', async () => {
  const css = Object.fromEntries(await Promise.all(['console.css', 'product-ui.css', 'usage.css', 'workspace.css', 'access.css', 'pages.css', 'operations.css'].map(async name => [name, await readFile(new URL('../admin/' + name, import.meta.url), 'utf8')])));
  assert.match(css['console.css'], /#notice\{position:fixed;right:24px;bottom:24px;z-index:80;[^}]*width:min\(440px,calc\(100vw - 32px\)\)/);
  assert.match(css['console.css'], /@media \(max-width:720px\)\{[\s\S]*?#notice\{right:16px;bottom:16px;left:16px;width:auto\}/, 'full width at the bottom on phones');
  // No other stylesheet moves it: put into the page flow while the right/bottom offsets still
  // apply, the box is pushed up over the top bar and left under the sidebar edge.
  for (const [name, text] of Object.entries(css)) if (name !== 'console.css') assert.doesNotMatch(text, /#notice\s*\{/, name + ' does not reposition the notice');
  assert.equal((css['console.css'].match(/#notice\{/g) ?? []).length, 2, 'the rule and its phone variant only');
  assert.doesNotMatch(css['console.css'], /#notice\{[^}]*(?:position:(?:relative|static|absolute)|top:)/);
});
