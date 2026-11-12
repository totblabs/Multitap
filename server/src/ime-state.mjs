// ime-state-v1: the browser Console types 한글 as 두벌식 keys and switches the
// VM's IME with its 한/영 key, so it must know the VM's real mode. VNC does not
// carry it; the Host reads the foreground window's Korean IME state in the
// signed-in console session (apps/windows_host/src/ime_state.hpp) when the
// Broker asks. Every read is a fresh question to the Host: nothing is kept or
// assumed between reads, and an unanswered question is "unknown".
//
// Broker -> Host  {type:'host.ime.query', protocol:'ime-state-v1', id}
// Host -> Broker  {type:'host.ime.state', protocol:'ime-state-v1', id, imeKorean?}
// imeKorean is true (한글), false (Latin letters) or absent (the Host cannot
// tell: signed out, locked, a secure desktop, an elevated window, no IME).
// No window title, text or process name is ever part of it.
import { randomUUID } from 'node:crypto';

export const imeStateProtocol = 'ime-state-v1';
export const imeQueryMs = 1500;
export const imeStateCapability = value => value?.imeStateProtocol === imeStateProtocol && value.service === true;

// The Host's answer: exactly these keys, else null (the caller drops the Host).
export function imeStateReply(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return null;
  const keys = Object.keys(value);
  if (keys.some(key => !['type', 'protocol', 'id', 'imeKorean'].includes(key))) return null;
  if (value.type !== 'host.ime.state' || value.protocol !== imeStateProtocol || typeof value.id !== 'string' || !/^[a-f0-9-]{36}$/.test(value.id)) return null;
  if (value.imeKorean !== undefined && typeof value.imeKorean !== 'boolean') return null;
  return { id: value.id, imeKorean: value.imeKorean ?? null };
}

export class ImeState {
  constructor(relay, { timeoutMs = imeQueryMs, setTimer = setTimeout, clearTimer = clearTimeout } = {}) {
    Object.assign(this, { relay, timeoutMs, setTimer, clearTimer });
    this.pending = new Map(); // host -> { id, promise, resolve, timer }
  }
  // true / false from the Host's answer to this question; null when unknown.
  // Questions that overlap share one answer (one question per Host at a time).
  query(desktopId) {
    const host = this.relay.hosts.get(desktopId);
    if (!this.relay.hostAuthenticated(host) || host.capabilitiesPending || !imeStateCapability(host.capabilities)) return Promise.resolve(null);
    const current = this.pending.get(host);
    if (current) return current.promise;
    const id = randomUUID();
    let resolve;
    const promise = new Promise(done => { resolve = done; });
    const entry = { id, promise, resolve, timer: null };
    entry.timer = this.setTimer(() => this.finish(host, entry, null), this.timeoutMs);
    entry.timer?.unref?.();
    this.pending.set(host, entry);
    if (!this.relay.send(host.ws, { type: 'host.ime.query', protocol: imeStateProtocol, id })) this.finish(host, entry, null);
    return promise;
  }
  // false: a malformed answer (the relay drops the Host for it).
  result(host, value) {
    const reply = imeStateReply(value);
    if (!reply || !imeStateCapability(host.capabilities)) return false;
    const entry = this.pending.get(host);
    // A late answer to a question that timed out is not a new state.
    if (entry && entry.id === reply.id) this.finish(host, entry, reply.imeKorean);
    return true;
  }
  hostLost(host) { const entry = this.pending.get(host); if (entry) this.finish(host, entry, null); }
  finish(host, entry, value) {
    if (this.pending.get(host) !== entry) return;
    this.pending.delete(host); this.clearTimer(entry.timer); entry.resolve(value);
  }
  close() { for (const [host, entry] of [...this.pending]) this.finish(host, entry, null); }
}
