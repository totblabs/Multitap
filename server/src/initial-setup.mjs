import { HttpError } from './errors.mjs';
import { newSetupCode, normalizeSetupCode, setupCodeHash, setupCodeMatches } from './verification-codes.mjs';

// One-time code for the first administrator setup. A
// fresh server's admin/admin session may only complete setup, and only with
// this code, which is printed to the server's own log (systemd journal /
// docker logs) and stored as a hash. Commissioned servers have no pending
// setup account, so they never issue or need one.
export const setupCodeSetting = 'initial_setup_code';
const defaultAnnounce = (code, guide = null) => {
  console.log(JSON.stringify({ service: 'multitap-broker', event: 'initial_setup_code', setupCode: code,
    message: 'First administrator setup code. Enter it in the admin console setup screen; restarting the server issues a new code.' }));
  // The address to open, right beside the code: the root URL leads to setup.
  if (guide) console.log(JSON.stringify({ service: 'multitap-broker', event: 'initial_setup_url', urls: guide.urls, message: guide.message }));
};

// What the setup line tells the operator to open. Only configured addresses
// are printed; a native fresh server cannot see its own LAN address (no
// interface enumeration), and a Docker one knows it only from .env.
// The line is Korean and English in one (docs/i18n.md): the operator may read
// it in docker logs or the journal, where the installer's language is unknown.
export function setupGuide({ bootstrapOnly, setupOrigin = null, allowedOrigins = [], deploymentKind = 'unknown', bootstrapPort = 18480 }) {
  const open = urls => ({ urls, message: '처음 설정: ' + urls.join(' 또는 ') + ' 를 브라우저로 여세요 / First setup: open ' + urls.join(' or ') + ' in a browser' });
  if (!bootstrapOnly) return allowedOrigins.length ? open(allowedOrigins) : null;
  if (setupOrigin) return open([setupOrigin]);
  if (deploymentKind === 'docker') return { urls: [], message: '처음 설정: .env에 MULTITAP_SERVER_ADDRESS=<서버 IP> 를 넣고 docker compose up -d 를 다시 실행하세요 / First setup: put MULTITAP_SERVER_ADDRESS=<server IP> in .env and run docker compose up -d again' };
  return { urls: [], message: '처음 설정: http://<서버 IP>:' + bootstrapPort + ' 를 브라우저로 여세요 / First setup: open http://<server IP>:' + bootstrapPort + ' in a browser' };
}

export class InitialSetupCode {
  constructor(store, { announce = defaultAnnounce, guide = null, attempts = 10, windowMs = 60000, now = Date.now } = {}) {
    Object.assign(this, { store, announce, guide, attempts, windowMs, now }); this.windows = new Map();
  }
  pending() { return !!this.store.db.prepare("SELECT 1 FROM users WHERE role='admin' AND disabled=0 AND must_change_credentials=1 LIMIT 1").get(); }
  issue() {
    const code = newSetupCode();
    this.store.db.prepare('INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value').run(setupCodeSetting, setupCodeHash(normalizeSetupCode(code)));
    try { this.announce(code, this.guide); } catch { /* The log line is best effort; a new start prints another code. */ }
  }
  // At startup: a pending setup gets a fresh code (any earlier one stops
  // working); otherwise no code may linger.
  startup() {
    if (this.pending()) this.issue();
    else this.store.db.prepare('DELETE FROM settings WHERE key=?').run(setupCodeSetting);
  }
  // Throws unless `value` is the current code. A setup that became pending
  // after startup (bootstrap reset-initial) gets its code issued here.
  // Attempts are limited per admin session (`key`): 60 random bits are not
  // guessable, and one hostile admin/admin session cannot lock out another.
  verify(value, key) {
    const stored = this.store.setting(setupCodeSetting);
    if (!stored) { this.issue(); throw new HttpError(403, 'setup_code_required'); }
    const now = this.now();
    let window = this.windows.get(key);
    if (!window || now - window.start >= this.windowMs) {
      if (this.windows.size >= 1000) for (const [name, value] of this.windows) if (now - value.start >= this.windowMs) this.windows.delete(name);
      if (this.windows.size >= 1000) throw new HttpError(429, 'too_many_setup_attempts');
      window = { start: now, count: 0 }; this.windows.set(key, window);
    }
    if (++window.count > this.attempts) throw new HttpError(429, 'too_many_setup_attempts');
    if (!setupCodeMatches(value, stored)) throw new HttpError(403, 'setup_code_invalid');
  }
}
