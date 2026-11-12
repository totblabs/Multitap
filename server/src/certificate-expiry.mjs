import { connect } from 'node:tls';
import { isIP } from 'node:net';
import { X509Certificate } from 'node:crypto';

// tls-expiry-v1: a read-only look at the certificate this installation
// presents, so administrators hear about its expiry 14 and 3 days ahead.
// Nothing is renewed, replaced or trusted here.
export const certificateWarningDays = [14, 3];
export const certificateAuditEvent = 'tls_certificate_expiring';
export const daysLeft = (expiresAt, now) => Math.ceil((expiresAt - now) / 86400000);
const fingerprint = value => String(value ?? '').replaceAll(':', '').toLowerCase();

// The leaf of a listener's PEM chain (a broker serving HTTPS itself).
export function pemCertificate(pem) {
  const leaf = new X509Certificate(pem);
  return { expiresAt: Date.parse(leaf.validTo), fingerprint: fingerprint(leaf.fingerprint256) };
}

// The certificate a client is shown at `origin` (a reverse proxy in front of
// the broker). Only the handshake happens; nothing is sent after it. An
// untrusted certificate is still read: its expiry is what matters here.
export function probeCertificate(origin, timeoutMs = 10000) {
  const url = new URL(origin), host = url.hostname.replace(/^\[|\]$/g, '');
  return new Promise((resolve, reject) => {
    let settled = false;
    const finish = (error, value) => { if (settled) return; settled = true; clearTimeout(timer); socket.destroy(); if (error) reject(error); else resolve(value); };
    const socket = connect({ host, port: Number(url.port) || 443, servername: isIP(host) ? undefined : host, rejectUnauthorized: false });
    const timer = setTimeout(() => finish(new Error('tls_probe_timeout')), timeoutMs);
    socket.on('error', error => finish(error));
    socket.once('secureConnect', () => {
      const peer = socket.getPeerCertificate(), expiresAt = Date.parse(peer?.valid_to);
      finish(Number.isFinite(expiresAt) ? null : new Error('tls_certificate_unreadable'), { expiresAt, fingerprint: fingerprint(peer?.fingerprint256) });
    });
  });
}

// A broker with its own TLS listener presents that certificate (the earliest
// expiry of them); otherwise an https:// public origin is checked through the
// network. Checked shortly after start and then every 6 hours.
export class CertificateMonitor {
  constructor({ store, listeners = [], origin = null, probe = probeCertificate, now = Date.now, intervalMs = 6 * 3600000, firstDelayMs = 30000 }) {
    Object.assign(this, { store, listeners, origin, probe, now, intervalMs, firstDelayMs, timer: null, closed: false, current: null, recorded: null });
  }
  source() { return this.listeners.length ? 'listener' : /^https:/.test(this.origin ?? '') ? 'origin' : null; }
  status() {
    const source = this.source();
    return !source ? { state: 'not_applicable' } : this.current ?? { state: 'unknown', source, checkedAt: null };
  }
  async check() {
    const source = this.source(), checkedAt = new Date(this.now()).toISOString();
    if (!source) return this.status();
    let certificate;
    try { certificate = source === 'listener' ? this.listeners.reduce((a, b) => (b.expiresAt < a.expiresAt ? b : a)) : await this.probe(this.origin); }
    catch { this.current = { state: 'unknown', source, checkedAt, errorCode: 'tls_check_failed' }; return this.current; }
    if (this.closed) return this.status();
    const days = daysLeft(certificate.expiresAt, this.now());
    // The nearest threshold reached; a check that starts inside 3 days records only that one.
    const threshold = Math.min(...certificateWarningDays.filter(limit => days <= limit));
    this.current = { state: days <= 0 ? 'expired' : threshold === 3 ? 'critical' : threshold === 14 ? 'warning' : 'ok', source, expiresAt: new Date(certificate.expiresAt).toISOString(), daysLeft: days, checkedAt };
    if (Number.isFinite(threshold)) {
      // Once per certificate and threshold, across restarts.
      const subject = `${threshold}:${this.current.expiresAt.slice(0, 10)}:${certificate.fingerprint.slice(0, 16)}`;
      if (this.recorded !== subject && !this.store.db.prepare('SELECT 1 FROM audit_events WHERE event=? AND subject=? LIMIT 1').get(certificateAuditEvent, subject)) this.store.audit(certificateAuditEvent, 'server', subject);
      this.recorded = subject;
    }
    return this.current;
  }
  start() { this.schedule(this.firstDelayMs); return this; }
  schedule(delay) {
    if (this.closed) return;
    this.timer = setTimeout(async () => { try { await this.check(); } catch { /* The next check tries again. */ } this.schedule(this.intervalMs); }, delay);
    this.timer.unref?.();
  }
  close() { this.closed = true; clearTimeout(this.timer); }
}
