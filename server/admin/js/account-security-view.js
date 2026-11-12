// Presentation only. The caller owns authenticated requests, setup tokens,
// operation identity, and the completed-result/retirement transition.
import { t } from './i18n.js';
let instance = 0;
const stages = new Set(['loading', 'summary', 'proof', 'setup', 'recovery', 'uncertain']);
const intents = new Set(['setup', 'disable', 'recovery']);

export function createAccountSecurityView({ document, accountName = '', onAction, onClose = () => {} }) {
  if (!document?.createElement || typeof onAction !== 'function') throw new TypeError('Account security view requires a document and action callback');
  const titleId = `account-security-title-${++instance}`;
  const element = document.createElement('dialog');
  element.className = 'account-security-dialog';
  element.setAttribute('aria-labelledby', titleId);
  let model = { stage: 'loading', busy: false, codes: null, secret: null };
  let destroyed = false, closed = true, generation = 0, dispatched = false;
  let internalCloses = 0;
  let revealed = false, saved = false, discardQuestion = false, proofMethod = 'code';

  const node = (tag, className = '', text = null) => {
    const n = document.createElement(tag);
    if (className) n.className = className;
    if (text !== null) n.textContent = text;
    return n;
  };
  const wipeInputs = () => { for (const n of element.querySelectorAll('input')) { n.value = ''; if (n.type === 'checkbox') n.checked = false; } };
  const wipeRenderedSecrets = () => { for (const n of element.querySelectorAll('[data-secret-value],[data-recovery-code]')) n.textContent = ''; };
  const hasCodes = () => Array.isArray(model.codes) && model.codes.length > 0;
  const completed = () => model.requiresLogin === true || model.stage === 'recovery';
  const wipeAll = () => { wipeInputs(); wipeRenderedSecrets(); model.secret = null; model.codes = null; saved = false; revealed = false; };
  const listen = (n, type, fn) => {
    const version = generation;
    n.addEventListener(type, event => { if (!destroyed && version === generation) fn(event); });
  };
  const showLocalError = message => {
    const target = element.querySelector('[data-view-message]');
    if (target) { target.textContent = message; target.hidden = false; target.classList.remove('is-done'); }
  };
  function call(action) {
    // Clear DOM fields before handing a transient payload to the controller.
    wipeInputs();
    if (destroyed || dispatched) return;
    dispatched = true;
    const version = generation;
    for (const b of element.querySelectorAll('[data-action]')) b.disabled = true;
    try {
      const pending = onAction(action);
      if (pending && typeof pending.catch === 'function') pending.catch(() => {
        if (!destroyed && version === generation) showLocalError(t('account-security-view.1'));
      });
    } catch {
      if (!destroyed && version === generation) showLocalError(t('account-security-view.1'));
    }
  }
  function hide() {
    closed = true;
    if (typeof element.close === 'function' && element.open) { internalCloses++; element.close(); }
    else element.removeAttribute('open');
  }
  function finish(kind) {
    if (destroyed || dispatched) return;
    wipeAll();
    ++generation;
    element.replaceChildren();
    hide();
    call({ kind });
  }
  function requestClose() {
    if (destroyed || closed) return;
    if (hasCodes()) {
      if (discardQuestion) { discardQuestion = false; draw(); return; }
      if (saved) { finish('finish'); return; }
      discardQuestion = true; draw(); return;
    }
    if (completed()) { finish('finish'); return; }
    wipeAll(); ++generation; element.replaceChildren(); hide();
    onClose();
  }
  const cancel = event => { if (event.cancelable) { event.preventDefault(); requestClose(); } };
  element.addEventListener('cancel', cancel);
  // A native CloseWatcher may emit a noncancelable cancel after asynchronous
  // work. Guard keyboard Escape before that default; never reopen forcibly.
  const escape = event => {
    if (!destroyed && !closed && hasCodes() && event.key === 'Escape') {
      event.preventDefault(); event.stopPropagation(); requestClose();
    }
  };
  element.addEventListener('keydown', escape, true);
  const nativeClose = () => {
    if (internalCloses > 0) { internalCloses--; return; }
    if (destroyed || closed) return;
    // Browser/OS forced close is privacy disposal, not saved acknowledgement
    // or an explicit discard action. The controller invalidates its owner.
    wipeAll(); ++generation; closed = true; element.replaceChildren(); onClose();
  };
  element.addEventListener('close', nativeClose);
  const pageHide = () => {
    if (destroyed || closed) return;
    wipeAll(); ++generation; element.replaceChildren(); hide(); onClose();
  };
  document.defaultView?.addEventListener('pagehide', pageHide);
  const leave = event => {
    if (!destroyed && !closed && hasCodes() && !saved) { event.preventDefault(); event.returnValue = ''; }
  };
  document.defaultView?.addEventListener('beforeunload', leave);

  function button(label, action, style = 'secondary', disabled = false) {
    const b = node('button', style, label); b.type = 'button';
    b.setAttribute('data-action', action.kind);
    if (action.intent) b.setAttribute('data-intent', action.intent);
    b.disabled = disabled || model.busy || dispatched;
    listen(b, 'click', () => { if (!b.disabled) { if (action.kind === 'finish' || action.kind === 'discard') finish(action.kind); else call(action); } });
    return b;
  }
  const paragraph = (text, className = '') => node('p', className, text);
  const actions = (...children) => { const n = node('div', 'as-actions'); n.append(...children); return n; };
  function input(label, name, { type = 'password', maxLength = 256, numeric = false } = {}) {
    const wrap = node('label', 'as-field', label);
    const n = node('input'); n.name = name; n.type = type; n.maxLength = maxLength;
    n.autocomplete = 'off'; n.spellcheck = false;
    n.setAttribute('autocapitalize', 'off'); n.setAttribute('autocorrect', 'off');
    n.setAttribute('data-field', name);
    if (numeric) n.setAttribute('inputmode', 'numeric');
    n.disabled = model.busy || dispatched;
    wrap.append(n); return { wrap, input: n };
  }
  function proof(body) {
    const labels = { setup: t('account-security-view.2'), disable: t('account-security-view.3'), recovery: t('account-security-view.4') };
    body.append(paragraph(model.intent === 'recovery'
      ? t('account-security-view.5')
      : model.intent === 'disable' ? t('account-security-view.6')
      : t('account-security-view.7')));
    if (model.intent !== 'setup') body.append(paragraph(t('account-security-view.8'), 'as-impact'));
    const form = node('form', 'as-form'); form.noValidate = true; form.autocomplete = 'off';
    const password = input(t('account-security-view.9'), 'currentPassword'); form.append(password.wrap);
    let factor;
    if (model.proofNeedsFactor) {
      const modes = node('div', 'as-methods'); modes.setAttribute('role', 'group'); modes.setAttribute('aria-label', t('account-security-view.10'));
      for (const [key, label] of [['code', t('account-security-view.11')], ['recoveryCode', t('account-security-view.12')]]) {
        const b = node('button', 'secondary', label); b.type = 'button'; b.disabled = model.busy || dispatched;
        b.setAttribute('aria-pressed', String(proofMethod === key)); b.setAttribute('data-method', key);
        listen(b, 'click', () => { if (b.disabled) return; proofMethod = key; draw(); }); modes.append(b);
      }
      form.append(modes);
      factor = input(proofMethod === 'code' ? t('account-security-view.13') : t('account-security-view.14'), proofMethod,
        { type: proofMethod === 'code' ? 'text' : 'password', maxLength: proofMethod === 'code' ? 6 : 64, numeric: proofMethod === 'code' });
      form.append(factor.wrap);
      if (proofMethod === 'recoveryCode') form.append(paragraph(t('account-security-view.15'), 'as-hint'));
    }
    const text = model.intent === 'disable' ? t('account-security-view.16') : model.intent === 'recovery' ? t('account-security-view.17') : t('account-security-view.18');
    const submit = node('button', model.intent === 'disable' ? 'danger' : 'primary', model.busy ? t('common.30') : text);
    submit.type = 'submit'; submit.disabled = model.busy || dispatched; submit.setAttribute('data-action', 'submitProof');
    form.append(actions(submit));
    listen(form, 'submit', event => {
      event.preventDefault(); if (model.busy || dispatched) return;
      const currentPassword = password.input.value, value = factor?.input.value;
      wipeInputs();
      if (!currentPassword || currentPassword.length > 256) { showLocalError(t('account-security-view.19')); return; }
      if (factor && (proofMethod === 'code' ? !/^[0-9]{6}$/.test(value) : !value)) { showLocalError(proofMethod === 'code' ? t('account-security-view.20') : t('account-security-view.21')); return; }
      call({ kind: 'submitProof', currentPassword, ...(factor ? { [proofMethod]: value } : {}) });
    });
    body.append(form); return labels[model.intent] || labels.setup;
  }
  function setup(body) {
    body.append(paragraph(t('account-security-view.22')));
    const reveal = node('button', 'secondary', revealed ? t('account-security-view.23') : t('account-security-view.24')); reveal.type = 'button';
    reveal.setAttribute('data-reveal-secret', ''); reveal.setAttribute('aria-expanded', String(revealed));
    reveal.disabled = !model.secret || model.busy || dispatched;
    listen(reveal, 'click', () => { if (!reveal.disabled) { revealed = !revealed; draw(); } }); body.append(reveal);
    if (revealed) {
      // In groups of four, as it is typed into the authenticator app (the app takes it with or without the spaces).
      const key = node('code', 'as-secret-value', model.secret.replace(/(.{4})(?=.)/g, '$1 ')); key.setAttribute('data-secret-value', ''); body.append(key);
    }
    const at = model.expiresAt ? new Date(model.expiresAt) : null;
    // Until when the code can be entered, by the 24-hour clock like the rest of the console (the minute it ends in, never later).
    if (at && Number.isFinite(at.getTime())) body.append(paragraph(t('account-security-view.25', { hour: String(at.getHours()), minute: String(at.getMinutes()).padStart(2, '0') }), 'as-hint'));
    body.append(paragraph(t('account-security-view.26'), 'as-impact'));
    const form = node('form', 'as-form'); form.noValidate = true; form.autocomplete = 'off';
    const code = input(t('account-security-view.13'), 'code', { type: 'text', maxLength: 6, numeric: true }); form.append(code.wrap);
    const submit = node('button', 'primary', model.busy ? t('common.30') : model.enabled ? t('account-security-view.27') : t('account-security-view.28'));
    submit.type = 'submit'; submit.disabled = !model.secret || model.busy || dispatched; submit.setAttribute('data-action', 'submitSetup');
    form.append(actions(submit)); listen(form, 'submit', event => {
      event.preventDefault(); if (!model.secret || model.busy || dispatched) return;
      const value = code.input.value; wipeInputs();
      if (!/^[0-9]{6}$/.test(value)) { showLocalError(t('account-security-view.20')); return; }
      call({ kind: 'submitSetup', code: value });
    }); body.append(form); return t('account-security-view.29');
  }
  function result(body) {
    if (!hasCodes()) {
      body.append(paragraph(model.enabled === false ? t('account-security-view.30') : t('account-security-view.31')));
      body.append(paragraph(t('account-security-view.32')));
      body.append(actions(button(t('account-security-view.33'), { kind: 'finish' }, 'primary')));
      return t('account-security-view.34');
    }
    if (discardQuestion) {
      body.append(paragraph(t('account-security-view.35')));
      const back = node('button', 'primary', t('account-security-view.36')); back.type = 'button'; back.setAttribute('data-keep-codes', '');
      listen(back, 'click', () => { discardQuestion = false; draw(); });
      const discard = node('button', 'danger', t('account-security-view.37')); discard.type = 'button'; discard.setAttribute('data-discard-codes', '');
      listen(discard, 'click', () => finish('discard')); body.append(actions(back, discard)); return t('account-security-view.38');
    }
    body.append(paragraph(model.intent === 'recovery' ? t('account-security-view.39') : t('account-security-view.40')));
    const list = node('ol', 'as-codes'); list.setAttribute('aria-label', t('account-security-view.41'));
    for (const value of model.codes) { const li = node('li'); const code = node('code', '', value); code.setAttribute('data-recovery-code', ''); li.append(code); list.append(li); }
    body.append(list, paragraph(t('account-security-view.42'), 'as-impact'));
    if (model.intent === 'setup') body.append(paragraph(t('account-security-view.43'), 'as-hint'));
    const ack = node('label', 'as-saved'); const checkbox = node('input'); checkbox.type = 'checkbox'; checkbox.checked = saved;
    checkbox.setAttribute('data-saved-codes', ''); ack.append(checkbox, document.createTextNode(t('account-security-view.44')));
    const done = node('button', 'primary', t('account-security-view.33')); done.type = 'button'; done.disabled = !saved; done.setAttribute('data-finish-codes', '');
    listen(checkbox, 'change', () => { saved = checkbox.checked; done.disabled = !saved; });
    listen(done, 'click', () => { if (saved && !done.disabled) finish('finish'); });
    const discard = node('button', 'quiet', t('account-security-view.37')); discard.type = 'button'; discard.setAttribute('data-open-discard', '');
    listen(discard, 'click', () => { discardQuestion = true; draw(); });
    body.append(ack, actions(done, discard)); return model.intent === 'recovery' ? t('account-security-view.45') : t('account-security-view.46');
  }
  function draw() {
    wipeInputs(); wipeRenderedSecrets(); ++generation;
    element.replaceChildren();
    // The console's dialog head: the title with the account under it, and an
    // X (the word 닫기 where the document cannot draw one).
    const head = node('div', 'as-head'); const titles = node('div', 'as-titles'); const heading = node('h2'); heading.id = titleId; heading.tabIndex = -1;
    titles.append(heading); if (accountName) titles.append(paragraph(String(accountName), 'as-account'));
    const close = node('button', 'as-close'); close.type = 'button'; close.setAttribute('aria-label', t('account-security-view.47')); close.setAttribute('title', t('common.31')); close.setAttribute('data-close-security', '');
    if (typeof document.createElementNS === 'function') {
      const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg'), path = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      svg.setAttribute('viewBox', '0 0 24 24'); svg.setAttribute('aria-hidden', 'true'); path.setAttribute('d', 'M6 6l12 12M18 6L6 18'); svg.append(path); close.append(svg);
    } else close.textContent = t('common.31');
    listen(close, 'click', requestClose); head.append(titles, close);
    const body = node('div', 'as-body');
    // Over http there is one sentence and 닫기: the refusal is not shown as a failure with a retry.
    const httpOnly = model.stage === 'uncertain' && model.unavailable === true && !model.requiresLogin;
    const alert = node('p', 'as-message' + (model.stage === 'recovery' ? ' is-done' : ''), httpOnly ? '' : model.message || ''); alert.setAttribute('data-view-message', ''); alert.setAttribute('role', 'alert'); alert.hidden = httpOnly || !model.message;
    let title = t('account-security-view.48');
    if (model.stage === 'loading') body.append(paragraph(t('account-security-view.49')));
    else if (httpOnly) body.append(paragraph(t('account-security-view.httpsOnlyF3')));
    else if (model.stage === 'uncertain') {
      title = model.requiresLogin ? t('account-security-view.50') : t('account-security-view.51');
      body.append(paragraph(model.requiresLogin ? t('account-security-view.52') : t('account-security-view.53')));
      body.append(actions(model.requiresLogin ? button(t('account-security-view.33'), { kind: 'finish' }, 'primary') : button(t('common.32'), { kind: 'refresh' }, 'primary')));
    } else if (completed()) title = result(body);
    else if (model.stage === 'proof') title = proof(body);
    else if (model.stage === 'setup') title = setup(body);
    else if (model.enabled === true) {
      body.append(paragraph(t('account-security-view.54'), 'as-status'));
      if (Number.isInteger(model.recoveryCodesRemaining) && model.recoveryCodesRemaining >= 0) body.append(paragraph(t('account-security-view.55', { recoveryCodesRemaining: model.recoveryCodesRemaining }), 'as-hint'));
      body.append(actions(button(t('account-security-view.27'), { kind: 'begin', intent: 'setup' }), button(t('account-security-view.56'), { kind: 'begin', intent: 'disable' }, 'danger'), button(t('account-security-view.57'), { kind: 'begin', intent: 'recovery' }, 'primary')));
    } else if (model.enabled === false) {
      body.append(paragraph(t('account-security-view.58'), 'as-status'), paragraph(t('account-security-view.59')));
      body.append(actions(button(t('account-security-view.60'), { kind: 'begin', intent: 'setup' }, 'primary')));
    } else {
      body.append(paragraph(t('account-security-view.61')));
      body.append(actions(button(t('common.32'), { kind: 'refresh' }, 'primary')));
    }
    if (model.busy && model.stage !== 'loading' && !completed()) body.append(paragraph(t('account-security-view.62'), 'as-hint'));
    // 닫기 is the last button of the dialog, as in every window. Where leaving means signing in again (a
    // result), the buttons of that step are the way out and the X asks what they ask.
    if (!completed()) {
      const shut = node('button', 'secondary as-done', t('common.31')); shut.type = 'button'; shut.setAttribute('data-close-security-button', '');
      listen(shut, 'click', requestClose);
      const all = body.querySelectorAll('.as-actions'), last = all[all.length - 1];
      if (last) last.append(shut); else body.append(actions(shut));
    }
    // The message stays right above the buttons it explains (also inside a form).
    const rows = body.querySelectorAll('.as-actions'), row = rows[rows.length - 1];
    if (row?.parentNode) row.parentNode.insertBefore(alert, row); else body.append(alert);
    heading.textContent = title; element.append(head, body);
    if (!closed && element.open) heading.focus?.();
  }
  function render(next = {}) {
    if (destroyed) return;
    wipeInputs(); wipeRenderedSecrets();
    const nextIntent = intents.has(next.intent) ? next.intent : 'setup';
    if (model.stage !== 'proof' || next.stage !== 'proof' || model.intent !== nextIntent) proofMethod = 'code';
    model = {
      stage: stages.has(next.stage) ? next.stage : 'uncertain',
      enabled: typeof next.enabled === 'boolean' ? next.enabled : undefined,
      recoveryCodesRemaining: next.recoveryCodesRemaining,
      intent: nextIntent, busy: next.busy === true,
      message: typeof next.message === 'string' ? next.message : '',
      secret: typeof next.secret === 'string' ? next.secret : null,
      expiresAt: typeof next.expiresAt === 'string' ? next.expiresAt : null,
      codes: Array.isArray(next.codes) ? [...next.codes] : null,
      requiresLogin: next.requiresLogin === true, proofNeedsFactor: next.proofNeedsFactor === true, unavailable: next.unavailable === true,
    };
    if (hasCodes() && (model.codes.length !== 10 || model.codes.some(x => typeof x !== 'string' || !/^[a-fA-F0-9]{32}$/.test(x)) || new Set(model.codes).size !== 10)) {
      model.codes = null; model.stage = 'uncertain'; model.requiresLogin = true;
      model.message = t('account-security-view.63');
    }
    if (model.stage === 'recovery' && !hasCodes() && model.enabled !== false) {
      model.stage = 'uncertain'; model.requiresLogin = true;
      model.message = t('account-security-view.64');
    }
    dispatched = false; saved = false; revealed = false; discardQuestion = false;
    draw();
  }
  function open() {
    if (destroyed) return;
    if (!element.isConnected) document.body.append(element);
    closed = false;
    if (!element.open) {
      if (typeof element.showModal === 'function') element.showModal();
      else element.setAttribute('open', ''); // DOM-test/unsupported-browser presentation only.
    }
    element.querySelector('h2')?.focus?.();
  }
  function destroy() {
    if (destroyed) return;
    wipeAll(); destroyed = true; ++generation; dispatched = true;
    document.defaultView?.removeEventListener('beforeunload', leave);
    document.defaultView?.removeEventListener('pagehide', pageHide);
    element.removeEventListener('cancel', cancel);
    element.removeEventListener('keydown', escape, true);
    element.removeEventListener('close', nativeClose);
    hide(); element.replaceChildren(); element.remove();
  }
  render();
  return { element, open, render, destroy };
}
