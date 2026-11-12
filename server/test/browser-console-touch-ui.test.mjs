// The Console on a touch screen (Android). The grip is a
// mouse target; a touch screen gets 메뉴 and 키보드, and what is typed in the
// invisible field reaches the VM as keysyms. 한글 and Latin letters follow the
// VM's 한/영 mode as the Host reports it (ime-state-v1),
// never an assumption.
import { t, translatePage } from '../admin/console/i18n.js';
import { createConsoleAuthentication } from '../admin/console/console-auth.js';
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { runInNewContext } from 'node:vm';
import { parseHTML } from 'linkedom';

const html = await readFile(new URL('../admin/console/index.html', import.meta.url), 'utf8');
const leaseId = '0f8fad5b-d9cb-469f-a165-70867728950e';
const script = (await readFile(new URL('../admin/console/console.js', import.meta.url), 'utf8'))
  .replace(/^import .+;\r?\n/gm, '').replace(/\bboot\(\);\s*$/, '') +
  `\nglobalThis.__connect = value => { rfb = value; lease = { id: '${leaseId}' }; document.body.classList.add("console-connected"); };` +
  '\nglobalThis.__settled = () => !touchPumping && !touchQueue.length;';

// vm: the VM's IME as the Host would report it (true 한글, false Latin, null
// it cannot tell, undefined an older Host: the read fails). Right Alt flips it
// when `toggles` (a VM whose 한/영 key works) and the Host can tell.
function fixture({ coarse = true, userAgent = 'Fixture browser', vm = undefined, toggles = true } = {}) {
  const { document, window: dom } = parseHTML(html), window = new EventTarget();
  for (const dialog of document.querySelectorAll('dialog')) { dialog.open = false; dialog.showModal = () => { dialog.open = true; }; dialog.close = () => { dialog.open = false; }; }
  const ime = { state: vm, toggles, reads: 0, paths: [] };
  const fetch = async (path) => {
    if (ime.state !== undefined && path === `/console/api/desktops/pve-170/sessions/${leaseId}/ime`) {
      ime.reads++; ime.paths.push(path);
      return { status: 200, ok: true, json: async () => (ime.state === null ? {} : { imeKorean: ime.state }) };
    }
    return { status: 401, ok: false, json: async () => ({ error: { code: 'unauthorized' } }) };
  };
  // Real timers for the page's short waits; the long ones never hold the test.
  const timer = (fn, ms) => { const handle = setTimeout(fn, ms); if (ms > 1000) handle.unref?.(); return handle; };
  const context = {
    document, window, location: { pathname: '/console/pve-170', origin: 'https://fixture.invalid', protocol: 'https:', hostname: 'fixture.invalid' },
    navigator: { userAgent }, matchMedia: query => ({ matches: coarse && query === '(pointer: coarse)', addEventListener() {} }),
    fetch, AbortSignal, URL, createConsoleAuthentication,
    initLogging() {}, releaseCapture() {}, createHangulKeyAdapter: () => ({ reset() {}, handle() { return false; } }), t, translatePage,
    setTimeout: timer, clearTimeout,
  };
  runInNewContext(script, context);
  const $ = id => document.getElementById(id), sent = [];
  const remote = { viewOnly: false, _remoteCapsLock: null, sendKey: (keysym, code, down) => {
    sent.push([keysym, code, down]);
    if (code === 'AltRight' && ime.toggles && typeof ime.state === 'boolean') ime.state = !ime.state;
  }, focus() {}, blur() {}, disconnect() {} };
  const connect = () => context.__connect(remote);
  const type = value => { $('touch-keys').value = value; $('touch-keys').dispatchEvent(new dom.Event('input')); };
  const key = name => { const event = new dom.Event('keydown', { cancelable: true }); event.key = name; $('touch-keys').dispatchEvent(event); return event; };
  const click = id => $(id).dispatchEvent(new dom.Event('click', { cancelable: true }));
  const settled = async () => { for (let i = 0; i < 400; ++i) { await new Promise(resolve => setTimeout(resolve, 5)); if (context.__settled()) return; } assert.fail('the keys never finished'); };
  // The key presses as names: 한영 for Right Alt, ⌫ for Backspace, letters for 두벌식/US keys.
  const codes = () => sent.filter(e => e[2] !== false && e[1] !== 'ShiftLeft').map(e => e[1] === 'AltRight' ? '한영' : e[1] === 'Backspace' ? '⌫' : e[1].replace(/^Key/, '').toLowerCase());
  const note = () => $('touch-note').hidden ? '' : $('touch-note').textContent;
  return { $, document, sent, connect, type, key, click, dom, remote, ime, settled, codes, note };
}

test('a touch screen gets 메뉴 and 키보드; a mouse screen keeps the grip', () => {
  const touch = fixture();
  assert.ok(touch.document.body.classList.contains('touch'));
  assert.equal(touch.$('touch-menu').textContent.trim(), '메뉴');
  assert.equal(touch.$('touch-keyboard').textContent.trim(), '키보드');
  assert.deepEqual([...touch.document.querySelectorAll('#touch-tools button')].map(b => b.id), ['touch-menu', 'touch-keyboard'], 'two floating buttons, as on Android');
  assert.equal(fixture({ coarse: false }).document.body.classList.contains('touch'), false);
  // The Android app's console view says so in its user agent.
  assert.ok(fixture({ coarse: false, userAgent: 'Mozilla/5.0 (Linux; Android 14) Multitap-ConsoleWebView/1' }).document.body.classList.contains('touch'));
});

test('메뉴 opens and folds the tools', () => {
  const f = fixture(); f.connect();
  f.click('touch-menu'); assert.ok(f.document.body.classList.contains('toolbar-open'));
  f.click('touch-menu'); assert.equal(f.document.body.classList.contains('toolbar-open'), false);
});

// What a US-layout Windows would type from the key events (code + Shift), so
// '!' must arrive as Shift+Digit1, not as a bare keysym QEMU types as '1'.
const usRow = { Backquote: '`~', Minus: '-_', Equal: '=+', BracketLeft: '[{', BracketRight: ']}', Backslash: '\\|', Semicolon: ';:', Quote: '\'"', Comma: ',<', Period: '.>', Slash: '/?', Space: '  ' };
const usDigits = ')!@#$%^&*(';
function typed(events, caps = false) {
  let shift = false, out = '';
  for (const [keysym, code, down] of events) {
    if (code === 'ShiftLeft') { shift = down; continue; }
    if (down === false) continue;
    if (code === 'Backspace') out = out.slice(0, -1);
    else if (code === 'Enter') out += '\n';
    else if (/^Key[A-Z]$/.test(code)) out += shift !== caps ? code[3] : code[3].toLowerCase();
    else if (/^Digit\d$/.test(code)) out += shift ? usDigits[Number(code[5])] : code[5];
    else if (usRow[code]) out += usRow[code][shift ? 1 : 0];
    else out += `<${code ?? keysym}>`;
  }
  return out;
}

test('typed text reaches the VM as the right keys, Shift included', async () => {
  const f = fixture(); f.connect(); f.click('touch-keyboard');
  assert.equal(f.$('touch-keys').value, '  ');
  f.type('  aB1!@Z?'); await f.settled(); assert.equal(typed(f.sent), 'aB1!@Z?');
  f.click('touch-keyboard'); f.sent.length = 0; f.type(' '); await f.settled(); assert.equal(typed([['x', 'KeyX', true], ...f.sent]), '', 'a deleted rest character is Backspace');
  assert.equal(f.$('touch-keys').value, '  ', 'the field is refilled after a Backspace into the rest');
  f.sent.length = 0; assert.equal(f.key('Enter').defaultPrevented, true); f.key('ArrowLeft'); f.key('Unidentified'); await f.settled();
  assert.deepEqual(f.sent.map(e => e[1]), ['Enter', 'ArrowLeft']);
});

test('a phone keyboard that composes and corrects a word sends only the difference', async () => {
  const f = fixture({ vm: false }); f.connect(); f.click('touch-keyboard');
  const compose = (name) => f.$('touch-keys').dispatchEvent(new f.dom.Event(name));
  compose('compositionstart');
  f.type('  t'); f.type('  te'); f.type('  teh');
  f.type('  the'); // the keyboard corrects the word it is composing
  compose('compositionend');
  f.type('  the '); f.type('  the w');
  await f.settled(); assert.equal(typed(f.sent), 'the w');
});

// Caps Lock in the VM would otherwise turn the case of every letter around.
test('Caps Lock on in the VM turns Shift around for letters only', async () => {
  const f = fixture({ vm: false }); f.connect(); f.remote._remoteCapsLock = true; f.click('touch-keyboard');
  f.type('  aB1!'); await f.settled(); assert.equal(typed(f.sent, true), 'aB1!');
});

test('Host says 한글: 한글 goes as 두벌식 keys with no 한/영 key', async () => {
  const f = fixture({ vm: true }); f.connect(); f.click('touch-keyboard'); await f.settled();
  assert.ok(f.ime.reads >= 1, 'the keyboard opening reads the Host');
  f.type('  한글'); await f.settled();
  assert.deepEqual(f.codes(), ['g', 'k', 's', 'r', 'm', 'f']);
  assert.equal(f.note(), '');
  // Shifted jamo (ㅃ ㅉ ㄸ ㄲ ㅆ ㅒ ㅖ) hold Shift; Caps Lock does not flip them.
  f.click('touch-keyboard'); f.sent.length = 0; f.remote._remoteCapsLock = true; f.type('  까'); await f.settled();
  assert.deepEqual(f.sent.map(e => [e[1], e[2]]), [['ShiftLeft', true], ['KeyR', true], ['KeyR', false], ['ShiftLeft', false], ['KeyK', true], ['KeyK', false]]);
});

test('Host says English: one 한/영, confirmed by the Host, then 한글; Latin letters switch back the same way', async () => {
  const f = fixture({ vm: false }); f.connect(); f.click('touch-keyboard'); await f.settled();
  const before = f.ime.reads;
  f.type('  한글'); await f.settled();
  assert.deepEqual(f.codes(), ['한영', 'g', 'k', 's', 'r', 'm', 'f']);
  assert.ok(f.ime.reads > before, 'the switch is confirmed by a new Host reading');
  assert.equal(f.ime.state, true);
  // Within the run nothing is asked again; Latin letters need the other mode.
  f.sent.length = 0; f.type('  한글 ok'); await f.settled();
  assert.deepEqual(f.codes(), ['space', '한영', 'o', 'k']);
  assert.equal(f.ime.state, false);
  // A syllable the phone is still composing: the VM loses one jamo per Backspace.
  f.click('touch-keyboard'); f.sent.length = 0;
  const compose = name => f.$('touch-keys').dispatchEvent(new f.dom.Event(name));
  compose('compositionstart'); f.type('  ㄱ'); f.type('  가'); f.type('  각'); compose('compositionend'); await f.settled();
  assert.deepEqual(f.codes(), ['한영', 'r', '⌫', 'r', 'k', '⌫', '⌫', 'r', 'k', 'r']);
  // Digits and symbols type the same in both modes: no 한/영 for them.
  f.sent.length = 0; f.type('  1!'); await f.settled();
  assert.equal(f.sent.some(e => e[1] === 'AltRight'), false);
});

test('a 한/영 switch the Host does not confirm types nothing in the wrong mode', async () => {
  const f = fixture({ vm: false, toggles: false }); f.connect(); f.click('touch-keyboard'); await f.settled();
  f.type('  한글 ok'); await f.settled();
  assert.deepEqual(f.codes(), ['한영'], 'one switch, then nothing: no 두벌식 keys as Latin letters, no space, no ok');
  assert.equal(f.note(), '한/영 전환이 확인되지 않아 입력을 멈췄어요. 다시 입력하세요.');
  assert.equal(f.$('touch-keys').value, '  ', 'the field starts again from what the VM has');
  // The next run reads the Host afresh (it still says English): Latin goes without 한/영.
  f.sent.length = 0; f.type('  ok'); await f.settled();
  assert.deepEqual(f.codes(), ['o', 'k']);
});

test('the Host cannot tell: Latin letters, digits and symbols go without 한/영; 한글 is refused with one line', async () => {
  for (const vm of [null, undefined]) {
    const f = fixture({ vm }); f.connect(); f.click('touch-keyboard'); await f.settled();
    f.type('  Pass1!'); await f.settled();
    assert.equal(typed(f.sent), 'Pass1!'); assert.equal(f.sent.some(e => e[1] === 'AltRight'), false);
    assert.equal(f.note(), '');
    f.click('touch-keyboard'); f.sent.length = 0; f.type('  a한b'); await f.settled();
    assert.deepEqual(f.codes(), ['a'], '한글 and what follows it are not sent');
    assert.equal(f.note(), '한글은 데스크톱에 로그인돼 있어야 보낼 수 있어요. 로그인돼 있으면 Host를 업데이트하세요.');
    assert.equal(f.$('touch-keys').value, '  ');
  }
});

test('there is no 한/영 전환 in the menu and no floating 한/영 button', () => {
  const f = fixture({ vm: false }); f.connect();
  assert.equal(f.$('touch-hangul'), null);
  assert.equal([...f.document.querySelectorAll('button')].some(b => /한\/영|Korean and English/.test(b.getAttribute('aria-label') ?? '') || /한\/영/.test(b.textContent)), false);
  assert.equal(t('console.html.62'), 'console.html.62', 'its string is gone too');
});

test('nothing is sent before the picture is connected', async () => {
  const f = fixture(); f.click('touch-keyboard'); f.type('  a'); f.key('Enter'); await f.settled();
  assert.deepEqual(f.sent, []);
});
