// Optional signin-stream-v1 policy only. No live advertisement, Windows proof
// acquisition, I/O, timers or ordinary-stream readiness change lives here.
import { randomBytes } from 'node:crypto';
import { isDeepStrictEqual } from 'node:util';

export const signinStreamProtocol = 'signin-stream-v1';
export const signinServiceProtocol = 'signin-service-v1';
const id = value => typeof value === 'string' && /^[a-f0-9]{32}$/.test(value);
const uuid = value => typeof value === 'string' && /^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/.test(value);
const integer = (value, maximum) => Number.isSafeInteger(value) && value >= 1 && value <= maximum;
const object = value => value !== null && typeof value === 'object' && !Array.isArray(value);
const exact = (value, fields) => object(value) && Reflect.ownKeys(value).length === fields.length && fields.every(key => Object.hasOwn(value, key) && Object.getOwnPropertyDescriptor(value, key)?.get === undefined);
const scope = value => exact(value, ['kind', 'epoch']) && value.kind === 'composite' && integer(value.epoch, 65535);
const copyScope = value => Object.freeze({ kind: 'composite', epoch: value.epoch });

export function parseSigninBinding(value) {
  if (!exact(value, ['protocol', 'phase', 'workerGeneration', 'transitionId', 'mediaScope']) ||
      value.protocol !== signinStreamProtocol || !['signin', 'desktop'].includes(value.phase) ||
      !integer(value.workerGeneration, 0xffffffff) || !id(value.transitionId) || !scope(value.mediaScope)) return null;
  return Object.freeze({ protocol: signinStreamProtocol, phase: value.phase, workerGeneration: value.workerGeneration,
    transitionId: value.transitionId, mediaScope: copyScope(value.mediaScope) });
}

// Bounded service attestation on the current authenticated Host socket. It is
// not an independent Windows observation; native Permit remains the final
// fresh capture/input authority. Only the production adapter owns this cache.
export class SigninServiceLease {
  #host; #session; #instance; #current; #authorized; #accepts; #now;
  #last = 0; #revision = 0; #receipt = null; #closed = false; #retiredGeneration = 0;
  constructor({ host, sessionId, serviceInstance, isCurrentHost, authorized, acceptsBinding, now = Date.now }) {
    if (!object(host) || !uuid(sessionId) || !id(serviceInstance) || [isCurrentHost, authorized, acceptsBinding, now].some(fn => typeof fn !== 'function')) throw Error('invalid_signin_service_context');
    this.#host = host; this.#session = sessionId; this.#instance = serviceInstance;
    this.#current = isCurrentHost; this.#authorized = authorized; this.#accepts = acceptsBinding; this.#now = now;
  }
  #time() {
    let now; try { now = this.#now(); if (!Number.isSafeInteger(now) || now < this.#last || this.#current(this.#host) !== true || this.#authorized() !== true) throw Error(); }
    catch { this.close(); return null; }
    this.#last = now;
    if (this.#receipt && now >= this.#receipt.deadline) this.close();
    return this.#closed ? null : now;
  }
  observe(host, value) {
    const now = this.#time();
    if (now === null || host !== this.#host || !exact(value, ['type','protocol','sessionId','serviceInstance','revision','binding','state','validForMs','renewAfterMs']) ||
        value.type !== 'host.signin.authority' || value.protocol !== signinServiceProtocol || value.sessionId !== this.#session || value.serviceInstance !== this.#instance ||
        !integer(value.revision, 0xffffffff) || value.revision <= this.#revision || !['current','withdrawn'].includes(value.state) ||
        !integer(value.validForMs, 1000) || !integer(value.renewAfterMs, 500) || value.renewAfterMs > value.validForMs) return false;
    const parsed = parseSigninBinding(value.binding);
    if (!parsed || parsed.workerGeneration <= this.#retiredGeneration) return false;
    try { if (this.#accepts(parsed) !== true) return false; } catch { return false; }
    this.#revision = value.revision;
    if (value.state === 'withdrawn') { this.close(); return true; }
    this.#receipt = { binding: parsed, deadline: now + value.validForMs }; return true;
  }
  current(binding, { sameDesktopWorker = false } = {}) {
    if (this.#time() === null || !this.#receipt) return false;
    if (isDeepStrictEqual(binding, this.#receipt.binding)) return true;
    const other = this.#receipt.binding;
    return sameDesktopWorker && binding.phase === 'desktop' && other.phase === 'desktop' && binding.protocol === other.protocol &&
      binding.workerGeneration === other.workerGeneration && binding.transitionId === other.transitionId;
  }
  get revision() { return this.#revision; }
  get closed() { this.#time(); return this.#closed; }
  retired(binding) { return binding.workerGeneration <= this.#retiredGeneration; }
  retire(binding) { this.#retiredGeneration = Math.max(this.#retiredGeneration, binding.workerGeneration); this.#receipt = null; }
  close() { this.#closed = true; this.#receipt = null; }
}

// Internal service eligibility facts, NOT a public Host JSON schema. In
// particular, an offer does not imply a session lease, baseline or pixels.
const eligibilityFields = ['revision', 'qualifiedBackend', 'currentService', 'ownedDevice', 'windowsState', 'outputs'];
const eligibility = value => exact(value, eligibilityFields) && integer(value.revision, 0xffffffff) &&
  value.qualifiedBackend === true && value.currentService === true && value.ownedDevice === true &&
  ['locked', 'no-user'].includes(value.windowsState) && value.outputs === 1;

export class SigninEligibilityOffers {
  #entries = new Map(); #revisions = new Map(); #lastNow = 0; #now; #current; #newId; #ttl;
  constructor({ isCurrentHost, now = Date.now, newId = () => randomBytes(16).toString('hex'), ttlMs = 5000 }) {
    if (typeof isCurrentHost !== 'function' || typeof now !== 'function' || typeof newId !== 'function' || !integer(ttlMs, 10000)) throw Error('invalid_signin_offer_policy');
    this.#current = isCurrentHost; this.#now = now; this.#newId = newId; this.#ttl = ttlMs;
  }
  #time() {
    const now = this.#now();
    if (!Number.isSafeInteger(now) || now < this.#lastNow) { this.#entries.clear(); return null; }
    this.#lastNow = now; return now;
  }
  #host(host) { try { return object(host) && this.#current(host) === true; } catch { return false; } }
  observe(host, facts) {
    const now = this.#time();
    if (now === null || !this.#host(host) || !eligibility(facts)) { this.#entries.delete(host); return null; }
    const previous = this.#entries.get(host);
    const highest = this.#revisions.get(host) ?? 0;
    if (facts.revision <= highest) {
      if (!previous || facts.revision !== previous.revision || facts.windowsState !== previous.windowsState || previous.expiresAtMs <= now) { this.#entries.delete(host); return null; }
      return this.admit(host, previous.availabilityId); // same proof cannot renew its receipt deadline
    }
    if (!this.#revisions.has(host) && this.#revisions.size >= 1024) return null;
    this.#revisions.set(host, facts.revision);
    const continuous = previous && previous.windowsState === facts.windowsState && previous.expiresAtMs > now;
    const availabilityId = continuous ? previous.availabilityId : this.#newId();
    if (!id(availabilityId) || !continuous && availabilityId === previous?.availabilityId) return null;
    const value = Object.freeze({ protocol: signinStreamProtocol, availabilityId, revision: facts.revision,
      windowsState: facts.windowsState, receivedAtMs: now, expiresAtMs: now + this.#ttl });
    this.#entries.set(host, value); return value;
  }
  admit(host, availabilityId) {
    const now = this.#time(), value = this.#entries.get(host);
    if (now === null || !this.#host(host)) { this.#entries.delete(host); return null; }
    return value && id(availabilityId) && value.availabilityId === availabilityId && value.expiresAtMs > now ? value : null;
  }
  current(host) { const entry = this.#entries.get(host); return entry ? this.admit(host, entry.availabilityId) : null; }
  withdraw(host, revision) {
    this.#entries.delete(host);
    if (integer(revision, 0xffffffff) && revision > (this.#revisions.get(host) ?? 0)) this.#revisions.set(host, revision);
  }
  forgetDisconnected(host) { if (this.#host(host)) return false; this.#entries.delete(host); this.#revisions.delete(host); return true; }
}

const bindingFields = ['protocol', 'sessionId', 'transitionId', 'workerGeneration', 'mediaScope'];
const envelope = (value, type, fields) => exact(value, ['type', ...bindingFields, ...fields]) && value.type === type &&
  value.protocol === signinStreamProtocol && uuid(value.sessionId) && id(value.transitionId) &&
  integer(value.workerGeneration, 0xffffffff) && scope(value.mediaScope);
const matches = (value, binding, sessionId) => value.sessionId === sessionId && value.protocol === binding.protocol &&
  value.transitionId === binding.transitionId && value.workerGeneration === binding.workerGeneration &&
  scope(value.mediaScope) && value.mediaScope.epoch === binding.mediaScope.epoch;

// Auth and native/service evidence arrive through trusted caller callbacks,
// never through peer-supplied `verified:true` fields. The future adapter must
// bind authenticated Host firstFrameId to its actual private frame proof.
export class SigninStreamPhase {
  #sessionId; #host; #authorized; #current; #phaseAuthority; #validateInfo; #verifyFrame; #validateInput; #now;
  #lastNow = 0; #deadline; #budget; #state = 'await_host_frame'; #binding; #active = null; #frameId = null; #frameInfo = null;
  #previousWorkerExitRequired = false; #usedTransitions;
  #ownerIdentity = {}; #resizing = false;
  constructor({ sessionId, hostBinding, binding, authorized, isCurrentHost, isCurrentPhaseAuthority, validateInfo, verifyHostFrame, validateInput,
    now = Date.now, budgetMs = 90000 }) {
    const parsed = parseSigninBinding(binding);
    if (!uuid(sessionId) || !object(hostBinding) || !parsed || !integer(budgetMs, 90000) ||
        [authorized, isCurrentHost, isCurrentPhaseAuthority, validateInfo, verifyHostFrame, validateInput, now].some(fn => typeof fn !== 'function')) throw Error('invalid_signin_phase_context');
    this.#sessionId = sessionId; this.#host = hostBinding; this.#binding = parsed;
    this.#authorized = authorized; this.#current = isCurrentHost; this.#phaseAuthority = isCurrentPhaseAuthority; this.#validateInfo = validateInfo;
    this.#verifyFrame = verifyHostFrame; this.#validateInput = validateInput; this.#now = now;
    const start = this.#time(); if (start === null) throw Error('invalid_signin_phase_clock');
    this.#budget = budgetMs; this.#deadline = start + budgetMs; this.#usedTransitions = new Set([parsed.transitionId]);
  }
  #time() {
    const now = this.#now();
    if (!Number.isSafeInteger(now) || now < this.#lastNow) { this.#state = 'invalidated'; return null; }
    this.#lastNow = now; return now;
  }
  #admitted() {
    const now = this.#time();
    try {
      if (now === null || this.#authorized() !== true || this.#current(this.#host) !== true) { this.#state = 'invalidated'; return false; }
      if (['await_host_frame', 'await_client_decode'].includes(this.#state) && now >= this.#deadline) this.#state = 'expired';
      return !['invalidated', 'expired', 'closed'].includes(this.#state);
    } catch { this.#state = 'invalidated'; return false; }
  }
  get state() { this.#admitted(); return this.#state; }
  get binding() { return this.#binding; }
  get deadlineMs() { return this.#deadline; }
  // Query the trusted service/native lifetime authority on each usable action.
  // Eligibility/initial waiting/preparation deliberately remain lease-free.
  #currentPhaseAuthority() {
    try { if (this.#phaseAuthority(this.#host, this.#binding, this.#sessionId) === true) return true; } catch {}
    this.#state = 'invalidated'; this.#active = null; this.#frameId = null; this.#frameInfo = null;
    return false;
  }
  prepare(value, host) {
    if (!this.#admitted() || host !== this.#host || !envelope(value, 'stream.phase.prepare', ['phase', 'timeoutMs']) ||
        !['signin', 'desktop'].includes(value.phase) || !integer(value.timeoutMs, 90000)) return false;
    if (['await_host_frame', 'await_client_decode'].includes(this.#state)) {
      return matches(value, this.#binding, this.#sessionId) && value.phase === this.#binding.phase && value.timeoutMs === this.#budget;
    }
    if (!this.#active || value.phase === this.#active.phase || value.sessionId !== this.#sessionId ||
        value.workerGeneration !== this.#active.workerGeneration + 1 || value.mediaScope.epoch !== this.#active.mediaScope.epoch + 1 ||
        this.#usedTransitions.has(value.transitionId)) return false;
    const parsed = parseSigninBinding({ protocol: value.protocol, phase: value.phase, workerGeneration: value.workerGeneration,
      transitionId: value.transitionId, mediaScope: value.mediaScope });
    if (!parsed) return false;
    this.#binding = parsed; this.#usedTransitions.add(parsed.transitionId); this.#frameId = null; this.#frameInfo = null; this.#resizing = false;
    this.#previousWorkerExitRequired = true; this.#budget = value.timeoutMs; this.#deadline = this.#lastNow + value.timeoutMs; this.#state = 'await_host_frame';
    return true;
  }
  prepareResize(mediaScope, { budgetMs = 90000 } = {}) {
    if (!this.#admitted() || !this.#active || !this.#state.endsWith('_active') || !scope(mediaScope) ||
        mediaScope.epoch !== this.#active.mediaScope.epoch + 1 || !integer(budgetMs, 90000)) return false;
    this.#binding = parseSigninBinding({ ...this.#active, mediaScope }); this.#frameId = null; this.#frameInfo = null;
    this.#previousWorkerExitRequired = false; this.#resizing = true; this.#budget = budgetMs; this.#deadline = this.#lastNow + budgetMs; this.#state = 'await_host_frame';
    return true;
  }
  hostInfo(value, host) {
    if (!this.#admitted() || host !== this.#host || !envelope(value, 'stream.phase.info', ['phase', 'firstFrameId', 'info']) ||
        value.phase !== this.#binding.phase || !matches(value, this.#binding, this.#sessionId) || !id(value.firstFrameId)) return false;
    if (!['await_host_frame', 'await_client_decode'].includes(this.#state) || !object(value.info) || value.info.epoch !== this.#binding.mediaScope.epoch ||
        (value.info.sessionId !== undefined && value.info.sessionId !== this.#sessionId) ||
        (value.phase === 'signin' && value.info.audio?.available === true)) return false;
    try {
      if (this.#validateInfo(value.info, value.phase) !== true) return false;
      if (this.#state === 'await_client_decode') return this.#currentPhaseAuthority() && value.firstFrameId === this.#frameId && isDeepStrictEqual(value.info, this.#frameInfo);
      if (this.#verifyFrame(host, value, {
        binding: this.#binding, assignedSessionId: this.#sessionId, previousWorkerExitRequired: this.#previousWorkerExitRequired,
      }) !== true || !this.#currentPhaseAuthority()) return false;
    } catch { return false; }
    this.#frameId = value.firstFrameId; this.#frameInfo = structuredClone(value.info); this.#state = 'await_client_decode'; return true;
  }
  decoded(value) {
    if (!this.#admitted() || this.#state !== 'await_client_decode' ||
        !envelope(value, 'stream.phase.decoded', ['firstFrameId']) || !matches(value, this.#binding, this.#sessionId) || value.firstFrameId !== this.#frameId || !this.#currentPhaseAuthority()) return false;
    this.#active = this.#binding; this.#resizing = false; this.#state = this.#active.phase + '_active'; return true;
  }
  input(value) {
    if (!this.#admitted() || !this.#state.endsWith('_active') || !this.#active ||
        !envelope(value, 'input.phase', ['input']) || !matches(value, this.#active, this.#sessionId) ||
        !object(value.input) || !['input', 'input.reset', 'input.fence'].includes(value.input.type) || !this.#currentPhaseAuthority()) return false;
    try { return this.#validateInput(value.input, this.#active.phase) === true; } catch { return false; }
  }
  redirectsAllowed() { return this.#admitted() && this.#state === 'desktop_active' && this.#currentPhaseAuthority(); }
  redirectOwner() { return this.redirectsAllowed() ? Object.freeze({ identity: this.#ownerIdentity, binding: this.#active }) : null; }
  retainsRedirectOwner(owner) {
    if (!this.#admitted() || owner?.identity !== this.#ownerIdentity || !this.#active || owner.binding.phase !== 'desktop' || this.#binding.phase !== 'desktop' ||
        owner.binding.workerGeneration !== this.#binding.workerGeneration || owner.binding.transitionId !== this.#binding.transitionId ||
        !(this.#state === 'desktop_active' || this.#resizing)) return false;
    try { if (this.#phaseAuthority(this.#host, this.#binding, this.#sessionId, { redirectOwner: true, sameDesktopWorker: this.#resizing }) === true) return true; } catch {}
    this.#state = 'invalidated'; return false;
  }
  close() { this.#state = 'closed'; this.#active = null; this.#frameId = null; this.#frameInfo = null; }
}
