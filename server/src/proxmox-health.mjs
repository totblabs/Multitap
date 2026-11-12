import { X509Certificate, createHash } from 'node:crypto';

// pve-health-v1: what the admin console says about the Proxmox connection
// before every desktop shows a generic error (vm-config-sweep items 2 and 7).
// - A Proxmox call (API or console WebSocket) refused on the certificate is
//   remembered for exactly the connection settings it used, and a later call
//   that gets through with them clears it. Kept in memory: after a restart the
//   next Proxmox read finds it again.
// - The pinned certificate (the setup script pins a node's own certificate
//   when it lacks the address) expires, or Proxmox renews it: both end every
//   call. Its expiry is read from the stored certificate itself, nothing is
//   fetched.
// The admin console (app.js, overview.js, system.js) shows both, and the
// token's expiry, from 30 days before, red from 7.

// The codes Node gives a TLS verification failure (the same test as Proxmox.call).
export const certificateFailure = error => /CERT|SELF_SIGNED|UNABLE_TO_VERIFY|ALTNAME/.test(error?.code ?? '');

// The connection settings a certificate decision belongs to: a new address,
// certificate or pin starts clean.
const trustKey = config => createHash('sha256').update(JSON.stringify([config?.endpoint ?? '', config?.ca ?? '', config?.fingerprint ?? ''])).digest('hex');

// expired: the certificate Proxmox presents is past its date.
// changed: a pinned connection met another certificate (Proxmox renewed or
//          regenerated it); the chain check usually refuses it before the pin
//          check does, so every non-expiry refusal under a pin counts.
// untrusted: no pin, and the certificate does not verify (CA, name).
export function certificateFailureKind(config, error) {
  if (error?.code === 'CERT_HAS_EXPIRED') return 'expired';
  return config?.fingerprint ? 'changed' : 'untrusted';
}

export class ProxmoxHealth {
  constructor({ now = Date.now } = {}) { this.now = now; this.failure = null; }
  // A refused handshake; anything else is not this module's business.
  observe(config, error) {
    if (!certificateFailure(error)) return;
    const key = trustKey(config), kind = certificateFailureKind(config, error), now = this.now();
    const since = this.failure?.key === key && this.failure.kind === kind ? this.failure.since : now;
    this.failure = { key, kind, since, lastAt: now };
  }
  // A handshake with these settings got through (any HTTP answer at all).
  reached(config) { if (this.failure?.key === trustKey(config)) this.failure = null; }
  // The failure of the settings stored now, or null.
  status(config) {
    const failure = this.failure;
    if (!config || !failure || failure.key !== trustKey(config)) return null;
    return { kind: failure.kind, since: new Date(failure.since).toISOString(), lastAt: new Date(failure.lastAt).toISOString() };
  }
}

// The pinned certificate's expiry, or null without a pin (or with one that
// does not belong to the stored certificate: that never connects anyway).
export function pinnedCertificate(config) {
  if (!config?.fingerprint || typeof config.ca !== 'string' || !config.ca) return null;
  try {
    const certificate = new X509Certificate(config.ca);
    if (certificate.fingerprint256 !== config.fingerprint) return null;
    const expiresAt = Date.parse(certificate.validTo);
    return Number.isFinite(expiresAt) ? { expiresAt: new Date(expiresAt).toISOString() } : null;
  } catch { return null; }
}
