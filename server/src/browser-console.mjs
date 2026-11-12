import { createHash } from 'node:crypto';
import { HttpError, fields } from './errors.mjs';
import { desktopId, tokenHash } from './security.mjs';
import { BrowserConsoleAssets } from './browser-console-assets.mjs';
import { ConsoleWebviews, consoleWebViewProtocol, consoleWebviewAttachCode } from './console-webviews.mjs';

const idPattern = '[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}';
const uuidPattern = '[a-fA-F0-9]{8}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{4}-[a-fA-F0-9]{12}';
// sessions/:id/ime (ime-state-v1): the VM's 한/영 mode, asked of the Host per read.
const desktopRoute = new RegExp('^/console/api/desktops/(' + idPattern + ')/(connect|sessions/(' + uuidPattern + ')(/ime)?)$');
const websocketRoute = new RegExp('^/console/ws/(' + uuidPattern + ')$');
const shellRoute = new RegExp('^/console/(' + idPattern + ')$');
const attachRoute = new RegExp('^/console/(' + idPattern + ')/attach$');
const tokenPattern = /^[A-Za-z0-9_-]{43}$/;
export const consoleCookieName = origin => '__Secure-multitap-console-' + createHash('sha256').update(origin).digest('hex').slice(0, 16);
export const consolePagePolicy = "default-src 'none'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self' data: blob:; font-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";
const cookie = (origin, token, clear = false) => `${consoleCookieName(origin)}=${token}; Path=/console; HttpOnly; Secure; SameSite=Strict${clear ? '; Max-Age=0' : ''}`;
const csrfScope = (origin, token) => 'console-v1:' + origin + ':' + token;
const mfaScope = (origin, challenge) => 'console-mfa:' + origin + ':' + challenge;

// What a person sees who opens the Console over plain HTTP: a card that says
// what to do, not the API's JSON. No script, no image, nothing from elsewhere;
// the page carries its own few styles (the policy below allows only those).
export const consoleHttpsPagePolicy = "default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'";
// One page per language (src/i18n.mjs, chosen from the request's Accept-Language).
const httpsPageText = {
  ko: ['HTTPS 주소로 열어 주세요', 'Console은 https://로 시작하는 주소에서만 열려요.', '주소를 모르면 관리자에게 물어보세요.'],
  en: ['Open it with an HTTPS address', 'The Console opens only at an address that starts with https://.', 'If you do not know the address, ask your administrator.'],
};
// The same card for an address a browser cannot open at all (http.mjs: a wrong
// or unregistered address, HTTP turned off, a page that does not exist, the
// first setup not done yet). Fixed text only; the API's callers keep the JSON.
const refusalPageText = {
  origin_not_allowed: {
    ko: ['이 주소로는 열 수 없어요', '서버에 등록된 주소로만 열 수 있어요.', '주소를 모르면 관리자에게 물어보세요.'],
    en: ['This address cannot be opened', 'The server opens only at its registered addresses.', 'If you do not know the address, ask your administrator.'] },
  http_disabled: {
    ko: ['HTTPS 주소로 열어 주세요', '이 서버는 https://로 시작하는 주소에서만 열려요.', '주소를 모르면 관리자에게 물어보세요.'],
    en: ['Open it with an HTTPS address', 'This server opens only at an address that starts with https://.', 'If you do not know the address, ask your administrator.'] },
  setup_pending: {
    ko: ['처음 설정을 먼저 마쳐 주세요', '이 서버는 아직 처음 설정 전이라 이 주소는 열 수 없어요.', '관리자 페이지(/admin/)에서 설정을 마치면 열려요.'],
    en: ['Finish the first setup first', 'This server is not set up yet, so this address cannot be opened.', 'It opens once the setup is finished in the admin console (/admin/).'] },
  not_found: {
    ko: ['페이지를 찾을 수 없어요', '주소를 다시 확인해 주세요.', '앱을 받으려면 /download 를 여세요.'],
    en: ['Page not found', 'Check the address again.', 'To get the apps, open /download.'] },
};
const httpsPage = (lang, text = httpsPageText[lang], title = 'Multitap Console') => Buffer.from(`<!doctype html>
<html lang="${lang}">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="robots" content="noindex">
<title>${title}</title>
<style>
:root{color-scheme:dark;--canvas:#0f1215;--surface:#15191d;--line:#272d33;--ink:#eceff1;--muted:#9ba5ad;--warn:#ffd24a;--warn-soft:rgba(255,210,74,.11)}
@media (prefers-color-scheme:light){:root{color-scheme:light;--canvas:#f4f5f7;--surface:#fff;--line:#e0e4e8;--ink:#161c21;--muted:#5b6770;--warn:#8f6200;--warn-soft:rgba(232,176,20,.15)}}
*{box-sizing:border-box}
body{margin:0;min-height:100dvh;display:grid;place-items:center;padding:16px;background:var(--canvas);color:var(--ink);font-family:Pretendard,"Segoe UI","Noto Sans KR","Malgun Gothic",system-ui,sans-serif;word-break:keep-all;overflow-wrap:anywhere}
main{width:min(26.5rem,100%);padding:28px;border:1px solid var(--line);border-radius:16px;background:var(--surface)}
.icon{display:grid;place-items:center;width:44px;height:44px;border-radius:12px;background:var(--warn-soft);color:var(--warn);font-size:22px;font-weight:600;line-height:1}
h1{margin:16px 0 6px;font-size:17px;font-weight:600;line-height:1.35;letter-spacing:-.2px}
p{margin:0;font-size:14px;line-height:1.6;color:var(--muted)}
p+p{margin-top:6px}
</style>
</head>
<body>
<main>
<div class="icon" aria-hidden="true">!</div>
<h1>${text[0]}</h1>
<p>${text[1]}</p>
<p>${text[2]}</p>
</main>
</body>
</html>
`, 'utf8');
const httpsPages = { ko: httpsPage('ko'), en: httpsPage('en') };
export const consoleHttpsPage = lang => httpsPages[lang === 'ko' ? 'ko' : 'en'];
const refusalPages = Object.fromEntries(Object.entries(refusalPageText).map(([kind, text]) => [kind, { ko: httpsPage('ko', text.ko, 'Multitap'), en: httpsPage('en', text.en, 'Multitap') }]));
// The page for one of the kinds above, or null (every other refusal stays JSON).
export const refusalPage = (kind, lang) => Object.hasOwn(refusalPages, kind) ? refusalPages[kind][lang === 'ko' ? 'ko' : 'en'] : null;

export class BrowserConsole {
  constructor({ store, vault, relay, sessionTtlMs, sessionMaxLifetimeMs, assetDirectory, webviewOptions }) {
    Object.assign(this, { store, vault, relay, sessionTtlMs, sessionMaxLifetimeMs });
    this.assets = new BrowserConsoleAssets(assetDirectory);
    this.webviews = new ConsoleWebviews(store, webviewOptions);
    relay.consoleAuthenticate = (hash, desktopId, origin, readOnly = false) => this.webviews.authenticate(hash, { desktopId, origin, readOnly })?.user ?? null;
    relay.consoleParentHash = hash => this.webviews.parentHash(hash);
    relay.consolePrincipalCleanup = () => this.webviews.prune();
  }
  isPath(path) { return path === '/console' || path.startsWith('/console/'); }
  isApiPath(path) { return ['/console/api/login', '/console/api/login/confirm', '/console/api/session', '/console/api/logout', '/console/api/mfa/verify'].includes(path) || /^\/console\/api\/mfa(?:\/(setup|enable|reauth|disable|recovery))?$/.test(path) || desktopRoute.test(path) || attachRoute.test(path); }
  isPreparationRead(req) { return req.method === 'GET' && !!desktopRoute.exec((req.url ?? '').split('?')[0])?.[3]; }
  validate(req, origin, { writing = false } = {}) {
    // A configured HTTPS label alone is not evidence of the browser's TLS hop.
    if (!origin.startsWith('https:') || !['direct', 'proxy'].includes(req.brokerTls)) throw new HttpError(403, 'console_https_required');
    const raw = req.url.split('?')[0];
    if (req.url.includes('?')) throw new HttpError(400, 'query_not_supported');
    if (raw.includes('%') || raw.includes('\\') || raw.split('/').some(part => part === '.' || part === '..')) throw new HttpError(400, 'invalid_console_path');
    if (req.headers.origin !== undefined && req.headers.origin !== origin) throw new HttpError(403, 'origin_not_allowed');
    if (writing && req.headers.origin !== origin) throw new HttpError(403, 'origin_required');
    if (!this.vault) throw new HttpError(503, 'console_unavailable');
  }
  authenticate(req, origin) {
    const name = consoleCookieName(origin), values = (req.headers.cookie ?? '').split(';').map(value => value.trim()).filter(value => value.startsWith(name + '='));
    const token = values.length === 1 ? values[0].slice(name.length + 1) : '';
    const hash = tokenPattern.test(token) ? tokenHash(token) : null;
    const resolved = hash ? this.webviews.authenticate(hash, { origin, readOnly: this.isPreparationRead(req) }) : null;
    const user = resolved?.user;
    if (!user) throw new HttpError(401, 'authentication_required');
    if (user.must_change_credentials) throw new HttpError(428, 'credentials_change_required');
    return { ...resolved, token, hash, user };
  }
  current(req, origin, principal, writing = false) {
    const current = this.authenticate(req, origin);
    if (current.hash !== principal.hash || current.user.id !== principal.user.id) throw new HttpError(401, 'authentication_required');
    if (writing && !this.vault.verifyCsrf(csrfScope(origin, current.token), req.headers['x-multitap-console-csrf'])) throw new HttpError(403, 'csrf_required');
    return current;
  }
  desktop(principal, id) {
    try { desktopId(id); } catch { throw new HttpError(400, 'invalid_desktop_id'); }
    if (principal.derived && principal.desktopId !== id) throw new HttpError(404, 'desktop_not_found');
    const desktop = this.store.assignedDesktop(principal.user.id, id);
    if (!desktop || desktop.kind !== 'vm') throw new HttpError(404, 'desktop_not_found');
    return desktop;
  }
  sessionBody(principal, origin) { return { user: { username: principal.user.username }, csrfToken: this.vault.csrf(csrfScope(origin, principal.token)), ...(principal.derived ? { consoleWebView: { protocol: consoleWebViewProtocol, desktopId: principal.desktopId, expiresAt: new Date(principal.expiresAt).toISOString() } } : {}) }; }
  async serve(req, res, path, { origin, headers }) {
    if (!this.isPath(path) || this.isApiPath(path)) return false;
    this.validate(req, origin);
    try {
      const principal = this.authenticate(req, origin);
      if (principal.derived && shellRoute.test(path)) this.desktop(principal, shellRoute.exec(path)[1]);
    } catch (error) { if (!['authentication_required', 'credentials_change_required'].includes(error.code)) throw error; }
    const relative = path.startsWith('/console/assets/') ? path.slice('/console/assets/'.length) : shellRoute.test(path) ? 'index.html' : null;
    if (!relative) throw new HttpError(404, 'not_found');
    if (!['GET', 'HEAD'].includes(req.method)) throw new HttpError(405, 'method_not_allowed');
    const asset = await this.assets.get(relative);
    res.writeHead(200, { ...headers, 'Content-Type': asset.mime + (/^(text\/|application\/json)/.test(asset.mime) ? '; charset=utf-8' : ''), 'Content-Length': asset.bytes.length, 'Content-Security-Policy': consolePagePolicy });
    res.end(req.method === 'HEAD' ? undefined : asset.bytes); return true;
  }
  establish(req, res, origin, session) {
    if (!session || !tokenPattern.test(session.token)) throw new HttpError(503, 'console_authentication_unavailable');
    const principal = { token: session.token, hash: tokenHash(session.token), user: session.user };
    const current = this.store.authenticate(principal.hash);
    if (!current || current.id !== session.user?.id || current.must_change_credentials) { this.store.logout(principal.hash); throw new HttpError(401, 'authentication_required'); }
    principal.user = current;
    // Re-login must retire the old console authority rather than leave it live.
    try { const old = this.authenticate(req, origin); if (old.hash !== principal.hash) this.webviews.logout(old.hash); } catch {}
    res.setHeader('Set-Cookie', cookie(origin, session.token));
    return this.sessionBody(principal, origin);
  }
  async handle(req, res, path, { origin, body, send, login, confirmLogin = null, mfaRoutes = null }) {
    if (!this.isApiPath(path)) return false;
    const writing = !['GET', 'HEAD'].includes(req.method);
    this.validate(req, origin, { writing });
    const attachment = attachRoute.exec(path);
    if (attachment) {
      if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
      const code = await consoleWebviewAttachCode(req);
      this.validate(req, origin, { writing: true });
      const principal = this.webviews.consume(code, attachment[1], origin);
      // Replacing a child never logs out an independent browser/native login.
      try { const old = this.authenticate(req, origin); if (old.derived) this.webviews.logout(old.hash); } catch {}
      res.writeHead(303, { Location: '/console/' + attachment[1], 'Set-Cookie': cookie(origin, principal.token), 'Cache-Control': 'no-store', 'Referrer-Policy': 'no-referrer', 'X-Content-Type-Options': 'nosniff', 'Content-Security-Policy': "default-src 'none'; frame-ancestors 'none'" });
      res.end(); this.relay.revalidate(); return true;
    }
    if (path === '/console/api/login') {
      if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
      const input = await body(req, 2048);
      let session;
      try { session = await login(input, false, 'console'); } finally { input.password = ''; }
      if (session?.mfa) {
        const mfa = session.mfa;
        if (mfa.protocol !== 'mfa-v1' || !tokenPattern.test(mfa.challengeToken) || typeof mfa.expiresAt !== 'string' || !Number.isFinite(Date.parse(mfa.expiresAt))) throw new HttpError(503, 'console_authentication_unavailable');
        send(202, { mfa: { protocol: 'mfa-v1', challengeToken: mfa.challengeToken, expiresAt: mfa.expiresAt, methods: ['totp', 'recovery'] }, csrfToken: this.vault.csrf(mfaScope(origin, mfa.challengeToken)) });
      } else send(200, this.establish(req, res, origin, session));
      return true;
    }
    if (path === '/console/api/login/confirm') {
      if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
      const input = await body(req, 1024);
      try {
        this.validate(req, origin, { writing: true });
        if (!confirmLogin) throw new HttpError(503, 'console_authentication_unavailable');
        const session = confirmLogin(input, 'browser-console');
        send(200, this.establish(req, res, origin, session));
      } finally { if (input && typeof input === 'object') input.challengeToken = ''; }
      return true;
    }
    if (path === '/console/api/mfa/verify') {
      if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
      if (!mfaRoutes) throw new HttpError(503, 'console_mfa_unavailable');
      const input = await body(req, 1024);
      try {
        fields(input, ['challengeToken', 'code', 'recoveryCode']);
        if (!tokenPattern.test(input.challengeToken) || !this.vault.verifyCsrf(mfaScope(origin, input.challengeToken), req.headers['x-multitap-console-csrf'])) throw new HttpError(403, 'csrf_required');
        mfaRoutes.guard();
        const session = await mfaRoutes.verify(input, false, 'console');
        send(200, this.establish(req, res, origin, session));
      } finally { input.challengeToken = ''; input.code = ''; input.recoveryCode = ''; }
      return true;
    }
    const principal = this.authenticate(req, origin);
    this.current(req, origin, principal, writing);
    const mfaMatch = /^\/console\/api\/mfa(?:\/(setup|enable|reauth|disable|recovery))?$/.exec(path);
    if (mfaMatch) {
      if (!mfaRoutes || principal.derived) throw new HttpError(403, 'authentication_required');
      const action = mfaMatch[1] ?? 'status';
      if (req.method !== (action === 'status' ? 'GET' : 'POST')) throw new HttpError(405, 'method_not_allowed');
      send(action === 'setup' ? 201 : 200, await mfaRoutes.manage(action, action === 'status' ? {} : await body(req, 1024), { user: principal.user, hash: principal.hash, purpose: 'console', admin: false })); return true;
    }
    if (path === '/console/api/session' && req.method === 'GET') {
      if (!principal.derived) this.store.renewSession(principal.hash, Date.now(), this.sessionTtlMs, this.sessionMaxLifetimeMs);
      send(200, this.sessionBody(principal, origin), false); return true;
    }
    if ((path === '/console/api/logout' && req.method === 'POST') || (path === '/console/api/session' && req.method === 'DELETE')) {
      if (req.method === 'DELETE' && (req.headers['transfer-encoding'] || Number(req.headers['content-length']) > 0)) throw new HttpError(400, 'unexpected_body');
      if (req.method === 'POST') fields(await body(req, 256), []);
      this.current(req, origin, principal, true);
      this.webviews.logout(principal.hash); this.store.audit('console_logout', principal.user.id, '');
      res.setHeader('Set-Cookie', cookie(origin, '', true)); send(204); return true;
    }
    const match = desktopRoute.exec(path);
    if (!match) throw new HttpError(405, 'method_not_allowed');
    const id = match[1]; this.desktop(principal, id);
    if (match[2] === 'connect' && req.method === 'POST') {
      const input = await body(req, 256); fields(input, ['takeover']);
      if (input.takeover !== undefined && input.takeover !== true) throw new HttpError(400, 'invalid_takeover');
      const current = this.current(req, origin, principal, true), desktop = this.desktop(current, id);
      const abort = new AbortController();
      const closed = () => { if (!res.writableEnded) abort.abort(); };
      res.once('close', closed);
      try {
        let grant;
        try { grant = await this.relay.console.create(current.user, current.hash, id, { origin, signal: abort.signal, profile: 'novnc' }); }
        catch (error) {
          // Only an actual retained stop barrier is pending cleanup. Active
          // streaming or unrelated operations keep their ordinary refusal.
          // '스트리밍 끊고 연결': the streams on this assigned VM end,
          // another account's too (same rule as stream takeover); their stop
          // barrier then answers console_cleanup_pending and the page connects again.
          const own = [...this.relay.sessions.values()].filter(session => session.desktopId === id);
          if (error?.code === 'console_busy' && input.takeover === true && own.length && this.relay.takeoverAllowed(current.user, id)) {
            // Not relay.takeover: it authenticates an app session token, and a
            // console session is its own (it answered desktop_not_found).
            for (const session of own) {
              // The ended stream is told who took it, never a name.
              if (session.userId !== current.user.id) session.takenBy = current.user.display_name || current.user.username;
              if (session.ws || this.relay.resume.held(session)) this.relay.stop(session, 'taken_over', session.userId === current.user.id ? 'console_same_account' : 'console_other_account');
              else this.relay.sessions.delete(session.id); // An unattached ticket never reached the Host.
            }
            try { this.relay.store.audit('session_taken_over', current.user.id, JSON.stringify({ desktopId: id, sessions: own.length, otherUser: own.some(session => session.userId !== current.user.id), via: 'console' })); } catch { /* Optional audit. */ }
            this.current(req, origin, current, true); this.desktop(current, id); send(202, { code: 'console_cleanup_pending' }); return true;
          }
          if (error?.code === 'console_busy' && this.relay.sessionStop.blocked(id, 'stream')) { this.current(req, origin, current, true); this.desktop(current, id); send(202, { code: 'console_cleanup_pending' }); return true; }
          throw error;
        }
        try { const fresh = this.current(req, origin, current, true); this.desktop(fresh, id); if (abort.signal.aborted || res.destroyed) throw new HttpError(409, 'console_cancelled'); }
        catch (error) { try { this.relay.console.cancel(current.user, current.hash, id, grant.id); } catch {} throw error; }
        send(200, { id: grant.id, wsPath: '/console/ws/' + grant.id, expiresAt: grant.expiresAt, desktop: { id, name: desktop.name }, clipboardPolicy: { textToRemote: grant.clipboardPolicy?.textToRemote === true, textToLocal: grant.clipboardPolicy?.textToLocal === true }, qemuConsolePreparation: grant.qemuConsolePreparation ?? { status: 'unknown' }, ...(grant.consoleHint === 'signin_other_display' ? { consoleHint: grant.consoleHint } : {}) });
      } finally { res.off('close', closed); }
      return true;
    }
    if (match[3] && req.method === 'GET') {
      if (req.headers['transfer-encoding'] || Number(req.headers['content-length']) > 0) throw new HttpError(400, 'unexpected_body');
      const current = this.current(req, origin, principal); this.desktop(current, id);
      if (match[4]) {
        const value = await this.relay.console.imeStatus(current.user, current.hash, id, match[3], origin);
        // The login or the assignment may have ended while the Host answered.
        const fresh = this.current(req, origin, current); this.desktop(fresh, id);
        send(200, value, false); return true;
      }
      send(200, this.relay.console.preparationStatus(current.user, current.hash, id, match[3], origin), false); return true;
    }
    if (match[3] && !match[4] && req.method === 'DELETE') {
      if (req.headers['transfer-encoding'] || Number(req.headers['content-length']) > 0) throw new HttpError(400, 'unexpected_body');
      const current = this.current(req, origin, principal, true); this.desktop(current, id);
      this.relay.console.cancel(current.user, current.hash, id, match[3]); send(204); return true;
    }
    throw new HttpError(405, 'method_not_allowed');
  }
  upgrade(req, socket, head, url, reject, origin) {
    if (!this.isPath(url.pathname)) return false;
    try {
      this.validate(req, origin, { writing: true });
      const match = websocketRoute.exec(url.pathname);
      if (!match) throw new HttpError(404, 'not_found');
      const { user, hash } = this.authenticate(req, origin);
      if (typeof this.relay.console.upgradeBrowser !== 'function') throw new HttpError(503, 'console_unavailable');
      this.relay.console.upgradeBrowser(req, socket, head, match[1], reject, { user, hash, origin });
    } catch (error) { reject(error instanceof HttpError ? error.status : 403, 'Forbidden'); }
    return true;
  }
}
