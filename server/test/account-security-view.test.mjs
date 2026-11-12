import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
const { parseHTML } = await import('linkedom');
const sourceUrl = new URL('../admin/js/account-security-view.js', import.meta.url);
const source = fs.readFileSync(sourceUrl, 'utf8');
// The module's own imports (its text, ./i18n.js) resolve next to the real file.
const { createAccountSecurityView } = await import('data:text/javascript;base64,' + Buffer.from(source.replace(/from '(\.[^']+)'/g, (_, path) => `from '${new URL(path, sourceUrl).href}'`)).toString('base64'));
const codes = Array.from({ length: 10 }, (_, i) => i.toString(16).padStart(32, '0')); // synthetic display fixtures only
const key = 'SYNTHETICMANUALSETUPKEY';
function fixture(options = {}) {
  const { document, Event } = parseHTML('<!doctype html><html><body></body></html>');
  const calls = []; let closed = 0;
  const view = createAccountSecurityView({ document, accountName: 'SYNTHETIC-ADMIN',
    onAction: action => { calls.push(action); return options.onAction?.(action, view); },
    onClose: () => { closed++; options.onClose?.(view); },
  });
  view.open();
  return { document, Event, view, calls, get closed() { return closed; },
    q: selector => view.element.querySelector(selector),
    click(selector) { const n = this.q(selector); assert.ok(n, selector); n.dispatchEvent(new Event('click', { bubbles: true })); return n; },
    submit() { this.q('form').dispatchEvent(new Event('submit', { bubbles: true, cancelable: true })); },
    cancel() { const e = new Event('cancel', { cancelable: true }); view.element.dispatchEvent(e); return e; },
  };
}
const recovery = (f, extra = {}) => f.view.render({ stage: 'recovery', enabled: true, intent: 'setup', codes, requiresLogin: true, ...extra });

test('unknown/unavailable status is not OFF and cannot start a mutation', () => {
  const f = fixture(); f.view.render({ stage: 'summary', accountId: 'fake', revision: 3, effectKnown: true });
  assert.match(f.view.element.textContent, /상태를 확인할 수 없어요/);
  assert.equal(f.q('[data-action=begin]'), null);
  assert.doesNotMatch(f.view.element.textContent, /사용하지 않고/);
  f.click('[data-action=refresh]'); assert.deepEqual(f.calls, [{ kind: 'refresh' }]); f.view.destroy();
});
for (const intent of ['setup', 'disable', 'recovery']) test(`known enabled summary emits only the selected ${intent} intent`, () => {
  const f = fixture(); f.view.render({ stage: 'summary', enabled: true, recoveryCodesRemaining: 7 });
  assert.match(f.view.element.textContent, /남은 복구 코드 7개/);
  f.click(`[data-intent=${intent}]`); assert.deepEqual(f.calls, [{ kind: 'begin', intent }]);
  f.click(`[data-intent=${intent}]`); assert.equal(f.calls.length, 1); f.view.destroy();
});
test('OFF only after a Boolean status and all text is rendered without markup injection', () => {
  const f = fixture(); f.view.render({ stage: 'summary', enabled: false, message: '<img src=x onerror=alert(1)>' });
  assert.match(f.view.element.textContent, /사용하지 않고/); assert.equal(f.q('img'), null);
  f.click('[data-intent=setup]'); assert.deepEqual(f.calls, [{ kind: 'begin', intent: 'setup' }]); f.view.destroy();
});
test('initial password proof has no invented factor fields and wipes DOM before callback', () => {
  let held;
  const f = fixture({ onAction: () => assert.equal(held.value, '') });
  f.view.render({ stage: 'proof', intent: 'setup', proofNeedsFactor: false });
  held = f.q('[data-field=currentPassword]'); held.value = ' synthetic password ';
  f.submit(); assert.deepEqual(f.calls, [{ kind: 'submitProof', currentPassword: ' synthetic password ' }]);
  assert.equal(f.q('[data-field=code]'), null); f.view.destroy();
});
test('factor proof rejects malformed TOTP locally and then sends exactly code', () => {
  const f = fixture(); f.view.render({ stage: 'proof', intent: 'disable', proofNeedsFactor: true });
  f.q('[data-field=currentPassword]').value = 'synthetic'; f.q('[data-field=code]').value = '12x'; f.submit();
  assert.equal(f.calls.length, 0); assert.match(f.q('[data-view-message]').textContent, /6자리/);
  assert.equal(f.q('[data-field=currentPassword]').value, '');
  f.q('[data-field=currentPassword]').value = 'synthetic'; f.q('[data-field=code]').value = '123456'; f.submit();
  assert.deepEqual(f.calls, [{ kind: 'submitProof', currentPassword: 'synthetic', code: '123456' }]); f.view.destroy();
});
test('switching proof methods clears old fields and recovery is exclusive without view normalization', () => {
  const f = fixture(); f.view.render({ stage: 'proof', intent: 'recovery', proofNeedsFactor: true });
  const password = f.q('[data-field=currentPassword]'), otp = f.q('[data-field=code]'); password.value = 'old'; otp.value = '123456';
  f.click('[data-method=recoveryCode]'); assert.equal(password.value, ''); assert.equal(otp.value, '');
  assert.match(f.view.element.textContent, /취소해도 다시 쓸 수 없어요/);
  const value = 'ABCD-ABCD-ABCD-ABCD-ABCD-ABCD-ABCD-ABCD';
  f.q('[data-field=currentPassword]').value = 'synthetic'; f.q('[data-field=recoveryCode]').value = value; f.submit();
  assert.deepEqual(f.calls, [{ kind: 'submitProof', currentPassword: 'synthetic', recoveryCode: value }]); f.view.destroy();
});
test('setup key is absent until explicit reveal; hiding clears detached secret nodes', () => {
  const f = fixture(); f.view.render({ stage: 'setup', secret: key, expiresAt: '2026-11-07T03:00:00Z' });
  assert.ok(!f.view.element.outerHTML.includes(key));
  const input = f.q('[data-field=code]'); input.value = '123456';
  f.click('[data-reveal-secret]'); assert.equal(input.value, '');
  // Shown in groups of four; the characters are the key's own.
  const revealed = f.q('[data-secret-value]'); assert.equal(revealed.textContent, 'SYNT HETI CMAN UALS ETUP KEY'); assert.equal(revealed.textContent.replaceAll(' ', ''), key); assert.equal(f.calls.length, 0);
  assert.equal(f.q('[data-reveal-secret]').textContent, '설정 키 숨기기');
  f.click('[data-reveal-secret]'); assert.equal(revealed.textContent, ''); assert.ok(!f.view.element.outerHTML.includes(key));
  f.q('[data-field=code]').value = '654321'; f.submit(); assert.deepEqual(f.calls, [{ kind: 'submitSetup', code: '654321' }]); f.view.destroy();
});
test('rerender wipes detached inputs and prevents retained old form callbacks from dispatching', () => {
  const f = fixture(); f.view.render({ stage: 'proof', intent: 'setup' });
  const oldInput = f.q('input'), oldForm = f.q('form'); oldInput.value = 'synthetic';
  f.view.render({ stage: 'summary', enabled: false }); assert.equal(oldInput.value, '');
  oldForm.dispatchEvent(new f.Event('submit', { cancelable: true })); assert.equal(f.calls.length, 0); f.view.destroy();
});
test('busy proof makes no duplicate action, but close remains available and clears inputs', () => {
  const f = fixture(); f.view.render({ stage: 'proof', intent: 'setup', busy: true });
  const input = f.q('input'); input.value = 'synthetic'; f.submit(); assert.equal(f.calls.length, 0);
  assert.match(f.view.element.textContent, /닫아도 보낸 요청/); f.click('[data-close-security]');
  assert.equal(input.value, ''); assert.equal(f.closed, 1); assert.equal(f.view.element.childNodes.length, 0); f.view.destroy();
});
test('code result cannot refresh or authorize and requires explicit saved acknowledgement', () => {
  const f = fixture(); recovery(f);
  assert.equal(f.q('[data-action=begin]'), null); assert.equal(f.q('[data-action=refresh]'), null);
  assert.equal(f.q('[data-finish-codes]').disabled, true); f.click('[data-finish-codes]'); assert.equal(f.calls.length, 0);
  assert.equal(f.q('input').type, 'checkbox'); assert.equal(f.q('[data-recovery-code]').textContent, codes[0]);
  assert.match(f.view.element.textContent, /새 번호/); f.view.destroy();
});
test('saved finish wipes codes and acknowledgement before delivery and sends no ordinary onClose', () => {
  let held;
  const f = fixture({ onAction: action => { assert.equal(action.kind, 'finish'); assert.equal(held.textContent, ''); } });
  recovery(f); held = f.q('[data-recovery-code]');
  const ack = f.q('[data-saved-codes]'); ack.checked = true; ack.dispatchEvent(new f.Event('change'));
  assert.equal(f.q('[data-finish-codes]').disabled, false); f.click('[data-finish-codes]');
  assert.deepEqual(f.calls, [{ kind: 'finish' }]); assert.equal(f.closed, 0); assert.equal(f.view.element.childNodes.length, 0); assert.equal(ack.checked, false); f.view.destroy();
});
test('native cancel guards unsaved codes; Back from discard returns before deliberate discard', () => {
  const f = fixture(); recovery(f, { intent: 'recovery' }); const held = f.q('[data-recovery-code]');
  assert.equal(f.cancel().defaultPrevented, true); assert.equal(f.calls.length, 0); assert.equal(held.textContent, '');
  assert.ok(f.q('[data-discard-codes]')); f.cancel(); assert.equal(f.q('[data-discard-codes]'), null); assert.equal(f.q('[data-recovery-code]').textContent, codes[0]);
  f.click('[data-open-discard]'); f.click('[data-discard-codes]'); assert.deepEqual(f.calls, [{ kind: 'discard' }]); assert.equal(f.closed, 0); f.view.destroy();
});
test('beforeunload warning is scoped to this unsaved result and removed on destroy', () => {
  const f = fixture(); recovery(f); const win = f.document.defaultView;
  const unsaved = new f.Event('beforeunload', { cancelable: true }); win.dispatchEvent(unsaved); assert.equal(unsaved.defaultPrevented, true);
  const ack = f.q('[data-saved-codes]'); ack.checked = true; ack.dispatchEvent(new f.Event('change'));
  const saved = new f.Event('beforeunload', { cancelable: true }); win.dispatchEvent(saved); assert.equal(saved.defaultPrevented, false);
  f.view.destroy(); const gone = new f.Event('beforeunload', { cancelable: true }); win.dispatchEvent(gone); assert.equal(gone.defaultPrevented, false);
});
test('keyboard Escape guards the result before noncancelable native CloseWatcher default', () => {
  const f = fixture(); recovery(f);
  const event = new f.Event('keydown', { bubbles: true, cancelable: true }); event.key = 'Escape';
  f.view.element.dispatchEvent(event);
  assert.equal(event.defaultPrevented, true); assert.ok(f.q('[data-discard-codes]')); assert.equal(f.calls.length, 0);
  f.view.destroy();
});
test('unavoidable native close clears codes and reports privacy disposal without fake save/discard', () => {
  const f = fixture(); recovery(f); const held = f.q('[data-recovery-code]');
  f.view.element.removeAttribute('open'); f.view.element.dispatchEvent(new f.Event('close'));
  assert.equal(held.textContent, ''); assert.equal(f.view.element.childNodes.length, 0); assert.equal(f.closed, 1);
  assert.deepEqual(f.calls, []); f.view.destroy();
});
test('pagehide clears an unsaved result before a back-forward cache can restore secrets', () => {
  const f = fixture(); recovery(f); const held = f.q('[data-recovery-code]');
  f.document.defaultView.dispatchEvent(new f.Event('pagehide'));
  assert.equal(held.textContent, ''); assert.equal(f.view.element.childNodes.length, 0);
  assert.equal(f.closed, 1); assert.deepEqual(f.calls, []); f.view.destroy();
});
test('rerendered/new code result always resets the saved acknowledgement', () => {
  const f = fixture(); recovery(f); const ack = f.q('[data-saved-codes]'); ack.checked = true; ack.dispatchEvent(new f.Event('change'));
  recovery(f); assert.equal(ack.checked, false); assert.equal(f.q('[data-saved-codes]').checked, false); assert.equal(f.q('[data-finish-codes]').disabled, true); f.view.destroy();
});
test('disable completion and ambiguous mutation expose only fresh-login action', () => {
  const f = fixture(); f.view.render({ stage: 'recovery', intent: 'disable', enabled: false, requiresLogin: true });
  assert.match(f.view.element.textContent, /2단계 인증을 껐어요/); assert.equal(f.q('[data-action=begin]'), null);
  f.view.render({ stage: 'uncertain', requiresLogin: true, enabled: true });
  assert.match(f.view.element.textContent, /변경이 적용됐을 수/); assert.doesNotMatch(f.view.element.textContent, /변경을 적용했어요/);
  assert.equal(f.q('[data-action=refresh]'), null); f.click('[data-action=finish]'); assert.deepEqual(f.calls, [{ kind: 'finish' }]); f.view.destroy();
});
test('malformed or duplicate code list is never displayed as a successful usable set', () => {
  const f = fixture(); recovery(f, { codes: [codes[0]] }); assert.equal(f.q('[data-recovery-code]'), null); assert.match(f.view.element.textContent, /목록을 확인하지 못했어요/);
  recovery(f, { codes: Array(10).fill(codes[0]) }); assert.equal(f.q('[data-recovery-code]'), null); assert.equal(f.q('[data-action=refresh]'), null); f.view.destroy();
});
test('destroy clears input and secret references, and late render/open cannot revive the view', () => {
  const f = fixture(); f.view.render({ stage: 'setup', secret: key }); f.click('[data-reveal-secret]');
  const secret = f.q('[data-secret-value]'), input = f.q('input'), oldForm = f.q('form'); input.value = '123456'; f.view.destroy();
  assert.equal(input.value, ''); assert.equal(secret.textContent, ''); f.view.render({ stage: 'setup', secret: key }); f.view.open();
  assert.equal(f.view.element.childNodes.length, 0); assert.equal(f.view.element.isConnected, false);
  oldForm.dispatchEvent(new f.Event('submit', { cancelable: true })); assert.equal(f.calls.length, 0);
});
test('late callback rejection cannot overwrite a newer result presentation', async () => {
  let reject; const held = new Promise((_, no) => { reject = no; });
  const f = fixture({ onAction: () => held }); f.view.render({ stage: 'summary', enabled: false }); f.click('[data-intent=setup]');
  recovery(f); reject(new Error('synthetic')); await Promise.resolve(); await Promise.resolve();
  assert.equal(f.q('[data-view-message]').hidden, true); assert.equal(f.q('[data-recovery-code]').textContent, codes[0]); f.view.destroy();
});
test('the setup step names its button 설정 키 보기 and says until when by the 24-hour clock', () => {
  const f = fixture(); const at = new Date(2026, 10, 12, 13, 5, 40);
  f.view.render({ stage: 'setup', secret: key, expiresAt: at.toISOString() });
  assert.equal(f.q('[data-reveal-secret]').textContent, '설정 키 보기');
  assert.match(f.view.element.textContent, /13시 05분까지 입력하세요 \(2분\)\./); assert.doesNotMatch(f.view.element.textContent, /오후|오전|유효기간/);
  f.view.destroy();
});
test('over http there is one sentence and 닫기: no failure title, no retry', () => {
  const f = fixture(); f.view.render({ stage: 'uncertain', unavailable: true, message: '보안 연결에서만 2단계 인증을 사용할 수 있어요.' });
  assert.equal(f.q('h2').textContent, '2단계 인증');
  assert.match(f.view.element.textContent, /https 주소로 접속하면 2단계 인증을 쓸 수 있어요\./);
  assert.doesNotMatch(f.view.element.textContent, /확인하지 못했어요|보안 연결에서만/);
  assert.equal(f.q('[data-action=refresh]'), null); assert.equal(f.q('[data-view-message]').hidden, true);
  assert.ok(f.q('[data-close-security-button]'));
  // Any other failed read keeps its title, its reason and 다시 확인.
  f.view.render({ stage: 'uncertain', message: '응답을 받지 못했어요.' });
  assert.equal(f.q('h2').textContent, '상태를 확인하지 못했어요'); assert.ok(f.q('[data-action=refresh]')); assert.equal(f.q('[data-view-message]').hidden, false);
  f.view.destroy();
});
test('what was done is not drawn as an error: the line of an applied change is marked done, a failure is not', () => {
  const f = fixture(); recovery(f, { message: '설정을 변경했어요. 다시 로그인해야 해요.' });
  assert.ok(f.q('[data-view-message]').classList.contains('is-done'));
  f.view.render({ stage: 'proof', intent: 'setup', message: '현재 비밀번호를 확인하세요.' });
  assert.equal(f.q('[data-view-message]').classList.contains('is-done'), false);
  f.view.destroy();
});
