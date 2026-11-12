import { MfaClientError, createMfaDeadline, mfaProofFields, normalizeMfaCode, readMfaStatus, readMfaSetup, readMfaReauth, readMfaChanged } from './mfa-auth.js';
import { t } from './i18n.js';

const RESULT_PRIVACY_MS = 30 * 60 * 1000;

// request(path,method,data,{accept}) calls accept({status,body}) synchronously
// while its exact response owner is current, before resolving the promise.
// retireAuthentication(ticket) MUST reset real auth and synchronously call
// controller.retire({preserveResult:ticket}); the ticket grants display only.
export function createAccountSecurityController({ document, createView, request, owner, isCurrent, origin, accountName, retireAuthentication, onClosed = () => {},
  paths = { status: '/mfa', setup: '/mfa/setup', enable: '/mfa/enable', reauth: '/mfa/reauth', disable: '/mfa/disable', recovery: '/mfa/recovery' }, now = Date.now, monotonic = () => performance.now(), timeoutMs = 15000, schedule = setTimeout, cancel = clearTimeout }) {
  let epoch = 0, operation = 0, view = null, captured = null, scope = null, setup = null, setupTimer = null, result = null, destroyed = false;
  let resultTimer = null, resultTimerRevision = 0, resultListeners = [];
  let model = { stage: 'loading', enabled: false, recoveryCodesRemaining: 0, intent: 'setup', busy: false, message: '', requiresLogin: false, proofNeedsFactor: false };
  const here = () => !destroyed && globalThis.document === document && scope === origin();
  const current = ticket => here() && ticket.epoch === epoch && ticket.operation === operation && !result && captured !== null && isCurrent(captured);
  const completed = ticket => result?.ticket === ticket && checkResultPrivacy();
  const render = () => { if (result && !checkResultPrivacy()) return; view?.render({ ...model, codes: result ? [...result.codes] : [] }); };
  const clearSetup = () => { clearTimeout(setupTimer); setupTimer = null; if (setup) { setup.setupToken = ''; setup.secret = ''; } setup = null; model.secret = ''; model.expiresAt = ''; };
  function expireSetup(candidate) {
    if (setup !== candidate) return;
    if (!here() || !captured || !isCurrent(captured)) { close(); return; }
    clearSetup(); model = { ...model, stage: 'proof', proofNeedsFactor: model.enabled, message: t('account-security-controller.1') }; render();
  }
  function privacyClock() {
    try { return { wall: now(), mono: monotonic() }; } catch { return { wall: NaN, mono: NaN }; }
  }
  function createResultPrivacy() {
    const { wall, mono } = privacyClock();
    return { lastWall: wall, lastMono: mono, wallDeadline: wall + RESULT_PRIVACY_MS, monoDeadline: mono + RESULT_PRIVACY_MS };
  }
  function resultRemaining() {
    const privacy = result?.privacy;
    if (!privacy) return Infinity; // An uncertain, code-free response is not a strict ACK.
    const { wall, mono } = privacyClock();
    if (![wall, mono, privacy.lastWall, privacy.lastMono, privacy.wallDeadline, privacy.monoDeadline].every(Number.isFinite) || wall < privacy.lastWall || mono < privacy.lastMono) return 0;
    privacy.lastWall = wall; privacy.lastMono = mono;
    return Math.max(0, Math.min(privacy.wallDeadline - wall, privacy.monoDeadline - mono));
  }
  function checkResultPrivacy() {
    if (!result) return false;
    if (!here() || resultRemaining() <= 0) { close(); return false; }
    return true;
  }
  function armResultPrivacy() {
    cancel(resultTimer); resultTimer = null;
    const revision = ++resultTimerRevision, owned = result;
    if (!owned?.privacy || !checkResultPrivacy()) return;
    const remaining = resultRemaining();
    if (remaining <= 0) { close(); return; }
    resultTimer = schedule(() => {
      if (result !== owned || revision !== resultTimerRevision) return;
      resultTimer = null;
      if (checkResultPrivacy()) armResultPrivacy();
    }, remaining);
    resultTimer?.unref?.();
  }
  function bindResultPrivacy() {
    if (!result?.privacy || !checkResultPrivacy()) return;
    const owned = result, element = view?.element;
    const listen = (target, type, callback) => {
      if (typeof target?.addEventListener !== 'function') return;
      target.addEventListener(type, callback, true);
      resultListeners.push(() => target.removeEventListener(type, callback, true));
    };
    const activity = event => {
      if (result !== owned || !checkResultPrivacy()) return;
      if (event.isTrusted !== true || !element?.contains?.(event.target)) return;
      // Modal backdrop input can be retargeted to DIALOG by the browser.
      // Spatial input must also originate within this view's current border box.
      if (event.type === 'pointerdown' || event.type === 'wheel') {
        let rect; try { rect = element.getBoundingClientRect?.(); } catch { return; }
        const { clientX: x, clientY: y } = event;
        if (!rect || ![x, y, rect.left, rect.top, rect.right, rect.bottom].every(Number.isFinite) || rect.right <= rect.left || rect.bottom <= rect.top || x < rect.left || x >= rect.right || y < rect.top || y >= rect.bottom) return;
      }
      // Expiry/rollback is checked BEFORE a genuine in-view input can reset it.
      owned.privacy.wallDeadline = owned.privacy.lastWall + RESULT_PRIVACY_MS;
      owned.privacy.monoDeadline = owned.privacy.lastMono + RESULT_PRIVACY_MS;
      armResultPrivacy();
    };
    for (const type of ['pointerdown', 'keydown', 'wheel']) listen(element, type, activity);
    const returned = () => { if (result === owned) checkResultPrivacy(); };
    listen(document, 'visibilitychange', returned);
    listen(document?.defaultView, 'pageshow', returned);
    listen(document?.defaultView, 'focus', returned);
    armResultPrivacy();
  }
  const clearResult = () => {
    ++resultTimerRevision; cancel(resultTimer); resultTimer = null;
    for (const remove of resultListeners) remove(); resultListeners = [];
    if (result) result.codes.fill(''); result = null;
  };
  function close() { const wasOpen = !!view; ++epoch; ++operation; clearSetup(); clearResult(); captured = null; view?.destroy(); view = null; scope = null; model = { stage: 'loading', enabled: false, recoveryCodesRemaining: 0, intent: 'setup', busy: false, message: '', requiresLogin: false, proofNeedsFactor: false }; if (wasOpen) { try { onClosed(); } catch { /* Focus is presentation, never authority. */ } } }
  function ensure() { if (!view) { view = createView({ document, accountName: accountName(), onAction: action => void act(action), onClose: close }); document.body.append(view.element); } }
  function fail(error, ticket, mutating) {
    if (!current(ticket)) return;
    model.busy = false;
    const typed = error instanceof MfaClientError ? error : new MfaClientError(error?.status, error?.code, { uncertain: mutating && !error?.status });
    model.message = typed.message;
    // Over http the server refuses every 2단계 인증 request (mfa_requires_https): that is not a failed read, and asking again cannot help.
    if (!mutating) { model = { ...model, stage: 'uncertain', enabled: undefined, recoveryCodesRemaining: undefined, requiresLogin: false, unavailable: typed.code === 'mfa_requires_https' }; }
    if (mutating && (typed.uncertain || !typed.status || typed.status >= 500)) { handoff({ enabled: model.enabled, codes: [] }, ticket, typed.message); return; }
    render();
  }
  async function send(path, method, data, decode, ticket, admit) {
    if (!current(ticket)) throw new MfaClientError(0, 'admin_session_changed');
    let timer, accepted = false, acceptedValue;
    const pending = request(path, method, data, { accept: reply => {
      if (!current(ticket)) throw new MfaClientError(0, 'admin_session_changed');
      const value = decode(reply.body, reply.status);
      accepted = true; acceptedValue = value;
      if (admit) admit(value);
      return value;
    } });
    try {
      const value = await Promise.race([pending, new Promise((_, reject) => { timer = setTimeout(() => reject(new MfaClientError(0, 'mfa_network_unknown', { uncertain: true })), timeoutMs); })]);
      if (!accepted || value !== acceptedValue) throw new MfaClientError(0, 'mfa_response_invalid', { uncertain: true });
      return value;
    }
    finally { clearTimeout(timer); }
  }
  function handoff(value, ticket, uncertainMessage = '') {
    if (!current(ticket)) return;
    const capability = Object.freeze(Object.create(null));
    clearSetup(); result = { ticket: capability, codes: [...value.codes], privacy: uncertainMessage ? null : createResultPrivacy() };
    model = { stage: uncertainMessage ? 'uncertain' : 'recovery', enabled: value.enabled, recoveryCodesRemaining: value.codes.length, intent: model.intent, busy: false, message: uncertainMessage || (value.enabled ? t('account-security-controller.2') : t('account-security-controller.3')), requiresLogin: true, proofNeedsFactor: false };
    retireAuthentication(capability, { uncertain: !!uncertainMessage });
    if (!completed(capability)) { close(); return; }
    render(); view?.open(); bindResultPrivacy();
  }
  async function open() {
    close(); if (destroyed) return;
    captured = owner(); scope = origin();
    if (!isCurrent(captured)) { close(); return; }
    ensure(); view.open(); model.busy = true; render();
    const ticket = { epoch, operation: ++operation };
    try { const status = await send(paths.status, 'GET', undefined, readMfaStatus, ticket); if (!current(ticket)) return; model = { ...model, ...status, stage: 'summary', busy: false, message: '' }; render(); }
    catch (error) { fail(error, ticket, false); }
  }
  async function act(action) {
    if (!action || typeof action.kind !== 'string' || destroyed) return;
    if (result) { if (['finish', 'discard'].includes(action.kind) && completed(result.ticket)) close(); return; }
    if (!captured || !here() || !isCurrent(captured)) { close(); return; }
    if (model.busy) return;
    if (action.kind === 'refresh') { await open(); return; }
    if (action.kind === 'begin') {
      if (model.stage !== 'summary' || !['setup', 'disable', 'recovery'].includes(action.intent) || (!model.enabled && action.intent !== 'setup')) return;
      clearSetup(); model = { ...model, stage: 'proof', intent: action.intent, message: '', proofNeedsFactor: model.enabled }; render(); return;
    }
    if (action.kind === 'finish' || action.kind === 'discard') { close(); return; }
    const ticket = { epoch, operation: ++operation };
    let input;
    try {
      if (action.kind === 'submitProof' && model.stage === 'proof') {
        input = mfaProofFields(action, model.proofNeedsFactor);
        model.busy = true; model.message = ''; render();
        // Existing factor proof and the mutation are two ordered server calls.
        // Retain password only within this explicit operation, then erase it.
        if (model.proofNeedsFactor) {
          await send(paths.reauth, 'POST', input, readMfaReauth, ticket);
          if (!current(ticket)) return;
          input.code = ''; input.recoveryCode = '';
        }
        const password = { currentPassword: input.currentPassword };
        try {
          if (model.intent === 'setup') {
            const candidate = await send(paths.setup, 'POST', password, (body, status) => readMfaSetup(body, status, now()), ticket);
            if (!current(ticket)) return;
            setup = { ...candidate, deadline: createMfaDeadline(candidate.expiresAt, { now, monotonic }) };
            const admitted = setup;
            setupTimer = setTimeout(() => expireSetup(admitted), admitted.deadline.remaining());
            model = { ...model, stage: 'setup', busy: false, secret: candidate.secret, expiresAt: candidate.expiresAt, message: '' }; render();
          } else {
            await send(paths[model.intent], 'POST', password, (body, status) => readMfaChanged(body, status, model.intent), ticket, value => handoff(value, ticket));
          }
        } finally { password.currentPassword = ''; }
      } else if (action.kind === 'submitSetup' && model.stage === 'setup' && setup) {
        const code = normalizeMfaCode(action.code);
        if (!setup.deadline.valid()) { expireSetup(setup); return; }
        if (!code) throw new MfaClientError(400, 'invalid_mfa_fields');
        input = { setupToken: setup.setupToken, code };
        model.busy = true; model.message = ''; render();
        await send(paths.enable, 'POST', input, (body, status) => readMfaChanged(body, status, 'setup'), ticket, value => handoff(value, ticket));
      }
    } catch (error) { fail(error, ticket, ['submitProof', 'submitSetup'].includes(action.kind)); }
    finally {
      if (input) for (const key of Object.keys(input)) input[key] = '';
      for (const key of ['currentPassword', 'code', 'recoveryCode']) if (Object.hasOwn(action, key)) { try { action[key] = ''; } catch { /* Caller may freeze its event DTO. */ } }
      if (current(ticket) && model.busy) { model.busy = false; render(); }
    }
  }
  const api = {
    open, act,
    retire({ preserveResult } = {}) {
      if (preserveResult && completed(preserveResult)) { ++operation; captured = null; return; }
      close();
    },
    destroy() { close(); destroyed = true; },
    hasCompletedResult: () => !!result && completed(result.ticket),
  };
  return api;
}
