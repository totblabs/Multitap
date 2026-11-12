import { HttpError } from './errors.mjs';
import { newToken, tokenHash } from './security.mjs';

export const consoleWebViewProtocol = 'console-webview-v1';
const tokenPattern = /^[A-Za-z0-9_-]{43}$/;

// One-use handoff codes and derived console principals exist only in memory.
// They never become independent Store sessions or renew the parent login.
export class ConsoleWebviews {
  constructor(store, { now = Date.now, codeMs = 30000, childMs = 3600000 } = {}) {
    this.store = store; this.now = now; this.codeMs = codeMs; this.childMs = childMs;
    this.codes = new Map(); this.children = new Map(); this.attempts = new Map();
  }
  parent(binding) {
    const user = this.store.authenticate(binding.parentHash);
    if (!user || user.id !== binding.userId || user.must_change_credentials) return null;
    const desktop = this.store.assignedDesktop(user.id, binding.desktopId);
    return desktop?.kind === 'vm' ? user : null;
  }
  prune() {
    const now = this.now();
    for (const [hash, entry] of this.codes) if (entry.expiresAt <= now || !this.parent(entry)) this.codes.delete(hash);
    for (const [hash, entry] of this.children) if (entry.expiresAt <= now || !this.parent(entry)) this.children.delete(hash);
    for (const [hash, entry] of this.attempts) if (now - entry.started >= 60000) this.attempts.delete(hash);
  }
  create(user, parentHash, desktopId, origin) {
    this.prune();
    const binding = { parentHash, userId: user.id, desktopId, origin };
    if (!this.parent(binding)) throw new HttpError(404, 'desktop_not_found');
    const now = this.now(), bucket = this.attempts.get(parentHash);
    if ((!bucket && this.attempts.size >= 512) || bucket?.count >= 10 || this.codes.size >= 256 || this.children.size >= 128) throw new HttpError(429, 'console_webview_limit');
    if (bucket) ++bucket.count; else this.attempts.set(parentHash, { started: now, count: 1 });
    // A repeated open replaces only an unused handoff for this exact owner.
    for (const [hash, old] of this.codes) if (old.parentHash === parentHash && old.desktopId === desktopId && old.origin === origin) this.codes.delete(hash);
    const code = newToken(), expiresAt = now + this.codeMs;
    const ownerId = tokenHash(code);
    this.codes.set(ownerId, { ...binding, ownerId, expiresAt });
    return { protocol: consoleWebViewProtocol, path: '/console/' + desktopId + '/attach', code, expiresAt: new Date(expiresAt).toISOString() };
  }
  consume(code, desktopId, origin) {
    const hash = tokenPattern.test(code) ? tokenHash(code) : null, binding = hash && this.codes.get(hash);
    if (!binding || binding.desktopId !== desktopId || binding.origin !== origin) throw new HttpError(403, 'console_webview_invalid');
    this.codes.delete(hash); // The exact, origin-scoped code is spent once.
    if (binding.expiresAt <= this.now()) throw new HttpError(403, 'console_webview_expired');
    const user = this.parent(binding);
    if (!user) throw new HttpError(401, 'authentication_required');
    this.prune(); if (this.children.size >= 128) throw new HttpError(429, 'console_webview_limit');
    const token = newToken(), childHash = tokenHash(token), expiresAt = Math.min(this.now() + this.childMs, user.expires_at);
    if (!Number.isFinite(expiresAt) || expiresAt <= this.now()) throw new HttpError(401, 'authentication_required');
    this.children.set(childHash, { ...binding, expiresAt });
    return { token, hash: childHash, user, derived: true, desktopId, origin, expiresAt };
  }
  authenticate(hash, { origin, desktopId, readOnly = false } = {}) {
    const child = this.children.get(hash);
    if (!child) {
      const user = this.store.authenticate(hash);
      return user ? { user, derived: false } : null;
    }
    if (child.expiresAt <= this.now()) { if (!readOnly) this.children.delete(hash); return null; }
    if ((origin !== undefined && origin !== child.origin) || (desktopId !== undefined && desktopId !== child.desktopId)) return null;
    const user = this.parent(child);
    if (!user) { if (!readOnly) this.children.delete(hash); return null; }
    return { user, derived: true, desktopId: child.desktopId, origin: child.origin, expiresAt: child.expiresAt };
  }
  parentHash(hash) {
    const child = this.children.get(hash);
    return child ? (this.authenticate(hash) ? child.parentHash : null) : hash;
  }
  logout(hash) {
    if (this.children.delete(hash)) return;
    this.store.logout(hash);
  }
  close(parentHash, origin, desktopId, ownerId) {
    const matches = entry => entry.parentHash === parentHash && entry.origin === origin &&
      (desktopId === undefined || entry.desktopId === desktopId) && (ownerId === undefined || entry.ownerId === ownerId);
    for (const [hash, entry] of this.codes) if (matches(entry)) this.codes.delete(hash);
    for (const [hash, entry] of this.children) if (matches(entry)) this.children.delete(hash);
  }
}

export async function consoleWebviewAttachCode(req) {
  if (req.aborted || req.destroyed) throw new HttpError(400, 'request_aborted');
  if ((req.headers['content-type'] ?? '').split(';')[0].trim().toLowerCase() !== 'application/x-www-form-urlencoded') throw new HttpError(415, 'console_webview_form_required');
  const maximum = 64;
  if (Number(req.headers['content-length']) > maximum) { req.resume(); throw new HttpError(413, 'body_too_large'); }
  const raw = await new Promise((resolve, reject) => {
    let length = 0; const chunks = [];
    const cleanup = () => { req.off('data', data); req.off('end', end); req.off('error', error); req.off('aborted', aborted); };
    const error = value => { cleanup(); reject(value); };
    const aborted = () => error(new HttpError(400, 'request_aborted'));
    const data = chunk => { length += chunk.length; if (length > maximum) { cleanup(); req.resume(); reject(new HttpError(413, 'body_too_large')); } else chunks.push(chunk); };
    const end = () => { cleanup(); resolve(Buffer.concat(chunks).toString('utf8')); };
    req.on('data', data); req.once('end', end); req.once('error', error); req.once('aborted', aborted);
  });
  const value = /^code=([A-Za-z0-9_-]{43})$/.exec(raw)?.[1];
  if (!value) throw new HttpError(400, 'console_webview_invalid');
  return value;
}
