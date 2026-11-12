import { HttpError, fields } from './errors.mjs';
import { requestLanguage } from './i18n.mjs';
import { tokenHash, hashPassword, verifyPassword } from './security.mjs';
import { clientDiagnostics } from './client-diagnostics.mjs';
import { activeSessions, enrolledHosts, streamStatsSessions } from './admin-monitoring.mjs';
import { connectionCounts } from './activity.mjs';
import { usageLive, usageEvents, usageSummaryShared } from './usage.mjs';
import { approvedPcSql } from './local-pc.mjs';
import { createHash } from 'node:crypto';
import { readRegisteredInventory } from './vm-relocate.mjs';
import { pinnedCertificate } from './proxmox-health.mjs';

const syncSequences = new WeakMap();
// Proxmox /access/permissions values describe propagation, not the grant: presence is authoritative.
function vmsPrivilege(permissions, privilege) {
  const own = permissions?.['/vms'];
  return !!own && !Array.isArray(own) && Object.hasOwn(own, privilege) && [0, 1, false, true].includes(own[privilege]);
}
// Clipboard and device permissions belong to each assignment
// (user x VM); account responses list them per assigned VM instead.
const publicUser = u => ({ id: u.id, username: u.username, displayName: u.username, notes: u.notes, isPrimaryAdmin: !!u.is_primary_admin, role: u.role, mustChangeCredentials: !!u.must_change_credentials });
// Catalog metadata of the latest Host installer plus each VM's update view.
function hostUpdateSummary({ latest, desktops }) {
  return { latest: latest ? { version: latest.version, filename: latest.filename, size: latest.size, sha256: latest.sha256, changelog: latest.changelog ?? '', ...(latest.releasedAt ? { releasedAt: latest.releasedAt } : {}) } : null, desktops };
}
function confirmedPassword(input) {
  if (typeof input.password !== 'string' || typeof input.passwordConfirmation !== 'string' || input.password !== input.passwordConfirmation) throw new HttpError(400, 'password_confirmation_mismatch');
  return input.password;
}
function expectedAccountId(input) {
  const value = input?.expectedAccountId;
  if (value === undefined) throw new HttpError(428, 'account_identity_required');
  if (typeof value !== 'string' || !/^[A-Za-z0-9_-]{1,128}$/.test(value)) throw new HttpError(400, 'invalid_account_identity');
  return value;
}

export function adminRoutes({ store, vault, proxmox, relay, power, hostRequests, notices = null, resources = null, login, mfaRoutes = null, passwordJob, body, backup = null, stream = () => { throw new HttpError(503, 'backup_unavailable'); }, publicOrigin, setupScriptSha256 = () => null, cookieScope = null, legacyCookieOrigin = null, accessRuntime = null, hostRemovalActivity = null, serverInfo, serverUpdate, initialSetup, certificates = null, agentAccess = { denied: () => false, reset: () => {} }, audioInventory = null, consoleAccess = { state: () => null, record: () => {} }, relocation = null }) {
  // The public installation scheme owns cookie policy, never the backend hop.
  const secureCookie = publicOrigin === undefined || new URL(publicOrigin).protocol === 'https:';
  const legacyCookieName = secureCookie ? '__Secure-multitap-admin' : 'multitap-admin-http';
  const cookieName = legacyCookieName + (cookieScope ? '-' + createHash('sha256').update(cookieScope).digest('hex').slice(0, 16) : '');
  const cookie = (token, maxAge) => `${cookieName}=${token}; Path=/api/v1/admin; HttpOnly;${secureCookie ? ' Secure;' : ''} SameSite=Strict; Max-Age=${maxAge}`;
  function managedVm(id) {
    if (typeof id !== 'string') throw new HttpError(400, 'invalid_desktop_id');
    const value = store.db.prepare("SELECT * FROM desktops WHERE id=? AND kind='vm' AND pve_vmid IS NOT NULL").get(id);
    if (!value) throw new HttpError(404, 'desktop_not_found');
    return value;
  }
  function managedHostDesktop(id) {
    return store.db.prepare(`SELECT d.* FROM desktops d WHERE d.id=? AND (${approvedPcSql})`).get(id) ?? managedVm(id);
  }
  function config() {
    const value = store.setting('proxmox');
    return value ? vault.open(value) : null;
  }
  function requireConfig() { const value = config(); if (!value) throw new HttpError(409, 'proxmox_not_configured'); return value; }
  function readStatus(key) { try { return JSON.parse(store.setting(key) ?? '{}'); } catch { return {}; } }
  function metadata() {
    const value = config();
    // The SHA-256 of /setup/proxmox.py as served to this address: the setup
    // command checks the download against it before running it as root.
    const script = { setupScriptSha256: setupScriptSha256() };
    if (!value) return { configured: false, state: 'unconfigured', ...script };
    const sync = readStatus('proxmox_sync'), expiry = readStatus('proxmox_token_status');
    const expiresAt = expiry.expirySource === 'proxmox' ? expiry.expiresAt : value.expiresAt ?? null;
    const [userId, tokenName] = value.tokenId.split('!');
    return { configured: true, endpoint: value.endpoint, tokenId: value.tokenId, userId, tokenName, hasSecret: true, ca: value.ca, fingerprint: value.fingerprint || '',
      state: sync.state ?? 'unchecked', checkedAt: sync.checkedAt ?? null, errorCode: sync.errorCode ?? null, vmCount: sync.count ?? null,
      unavailableCount: Number.isSafeInteger(sync.unavailable) && sync.unavailable >= 0 ? sync.unavailable : null,
      expiresAt, expirySource: expiry.expirySource === 'proxmox' ? 'proxmox' : expiresAt ? 'configured' : 'unknown',
      expiresInDays: expiresAt ? Math.max(0, Math.ceil((Date.parse(expiresAt) - Date.now()) / 86400000)) : null,
      tokenExpiresAt: expiry.tokenExpiresAt ?? null, accountExpiresAt: expiry.accountExpiresAt ?? null,
      accountExpiryKnown: !!expiry.accountExpiryKnown, expiryCheckedAt: expiry.expiryCheckedAt ?? null, expiryError: expiry.expiryError ?? null,
      // True while the broker's last QEMU guest-agent call was refused for lack of privilege.
      agentPrivilegeMissing: agentAccess.denied() === true,
      // pve-health-v1 (proxmox-health.mjs): the last Proxmox call refused on the
      // certificate with these settings, and the pinned certificate's expiry.
      certificateFailure: proxmox?.health?.status?.(value) ?? null, pinnedCertificate: pinnedCertificate(value),
      // Proxmox's latest answer about the integration's VM.Console at /vms
      // (http.mjs keeps it): every sync and the administrator's own check
      // update it. It is shown, and grants nothing.
      consolePrivilege: { true: 'granted', false: 'missing' }[consoleAccess.state()] ?? 'unknown', ...script };
  }
  return async (req, res, path, send) => {
    if (!path.startsWith('/api/v1/admin/')) return false;
    if (!vault) throw new HttpError(503, 'admin_unavailable');
    const writing = !['GET', 'HEAD'].includes(req.method);
    if (writing && req.headers.origin !== publicOrigin) throw new HttpError(403, 'origin_required');
    if (path === '/api/v1/admin/login' && req.method === 'POST') {
      const session = await login(await body(req), true);
      if (session.mfa) { send(202, session); return true; }
      res.setHeader('Set-Cookie', cookie(session.token, Math.floor(session.ttl / 1000)));
      send(200, { user: publicUser(session.user), csrfToken: vault.csrf(session.token), onboarding: store.onboarding(session.user) }); return true;
    }
    if (path === '/api/v1/admin/mfa/verify' && req.method === 'POST') {
      if (!mfaRoutes) throw new HttpError(503, 'mfa_unavailable');
      const input = await body(req, 1024);
      try {
        const session = mfaRoutes.verify(input, true);
        res.setHeader('Set-Cookie', cookie(session.token, Math.floor(session.ttl / 1000)));
        send(200, { user: publicUser(session.user), csrfToken: vault.csrf(session.token), onboarding: store.onboarding(session.user) }); return true;
      } finally { input.challengeToken = ''; input.code = ''; input.recoveryCode = ''; }
    }
    const cookies = (req.headers.cookie ?? '').split(';').map(v => v.trim());
    let values = cookies.filter(v => v.startsWith(cookieName + '='));
    if (cookieScope && publicOrigin === legacyCookieOrigin && values.length === 0) values = cookies.filter(v => v.startsWith(legacyCookieName + '=')).map(v => cookieName + v.slice(legacyCookieName.length));
    const token = values.length === 1 ? values[0].slice(cookieName.length + 1) : '';
    const user = /^[A-Za-z0-9_-]{43}$/.test(token) ? store.authenticate(tokenHash(token), Date.now(), 'admin') : null;
    if (!user) throw new HttpError(401, 'authentication_required');
    if (user.role !== 'admin') throw new HttpError(403, 'admin_required');
    if (writing && !vault.verifyCsrf(token, req.headers['x-csrf-token'])) throw new HttpError(403, 'csrf_required');
    const actor = user.id;
    const mfaMatch = /^\/api\/v1\/admin\/mfa(?:\/(setup|enable|reauth|disable|recovery))?$/.exec(path);
    if (mfaMatch) {
      if (!mfaRoutes) throw new HttpError(503, 'mfa_unavailable');
      const action = mfaMatch[1] ?? 'status';
      if (req.method !== (action === 'status' ? 'GET' : 'POST')) throw new HttpError(405, 'method_not_allowed');
      send(action === 'setup' ? 201 : 200, await mfaRoutes.manage(action, action === 'status' ? {} : await body(req, 1024), { user, hash: tokenHash(token), purpose: 'admin', admin: true })); return true;
    }
    // Body parsing, password jobs and upstream calls yield to other requests.
    // A request's original principal is not authority to mutate after that gap.
    const currentAdmin = ({ setup = false } = {}) => {
      const current = store.authenticate(tokenHash(token), Date.now(), 'admin');
      if (!current || current.id !== actor || current.role !== 'admin' || (!setup && current.must_change_credentials)) throw new HttpError(401, 'authentication_required');
      if (writing && !vault.verifyCsrf(token, req.headers['x-csrf-token'])) throw new HttpError(403, 'csrf_required');
      return current;
    };
    try {
      if (path === '/api/v1/admin/session' && req.method === 'GET') { send(200, { user: publicUser(user), csrfToken: vault.csrf(token), onboarding: store.onboarding(user) }); return true; }
      if (path === '/api/v1/admin/logout' && req.method === 'POST') {
        currentAdmin({ setup: true });
        store.logout(tokenHash(token)); store.audit('admin_logout', actor, ''); res.setHeader('Set-Cookie', cookie('', 0)); send(200, { ok: true }); return true;
      }
      if (path === '/api/v1/admin/setup' && req.method === 'POST') {
        if (!user.must_change_credentials) throw new HttpError(409, 'setup_not_required');
        const input = await body(req); fields(input, ['username', 'password', 'passwordConfirmation', 'setupCode']);
        currentAdmin({ setup: true });
        // The one-time code from the server log proves access to the server
        // itself; admin/admin alone no longer completes the first setup.
        try { initialSetup.verify(input.setupCode, tokenHash(token)); } catch (error) { input.password = ''; input.passwordConfirmation = ''; throw error; }
        const hashed = await passwordJob(() => hashPassword(confirmedPassword(input))); input.password = ''; input.passwordConfirmation = '';
        currentAdmin({ setup: true });
        store.completeInitialSetup(user.id, input.username, hashed);
        // `multitap code` reads the log: after this line the code above it is no longer the answer.
        console.log(JSON.stringify({ service: 'multitap-broker', event: 'initial_setup_done' }));
        res.setHeader('Set-Cookie', cookie('', 0)); send(200, { ok: true, requiresLogin: true }); return true;
      }
      // 백업·복원 (backup.mjs). Making a file needs the administrator's password
      // again (and the second factor of an account that has one). A restore is
      // also offered to the pending first setup, under exactly what protects
      // that setup: its session and the one-time code from the server log.
      if (path === '/api/v1/admin/backup' || path.startsWith('/api/v1/admin/backup/')) {
        if (!backup?.available) throw new HttpError(503, 'backup_unavailable');
        const setup = !!user.must_change_credentials, owner = tokenHash(token);
        const principal = () => currentAdmin({ setup });
        const wipe = input => { for (const key of ['password', 'passphrase', 'passphraseConfirmation', 'code', 'recoveryCode', 'setupCode']) if (key in input) input[key] = ''; };
        const proveSetup = input => { initialSetup.verify(input.setupCode, owner); currentAdmin({ setup: true }); };
        const reauthenticate = async input => {
          if (!backup.limits.reauthentication.take(actor)) throw new HttpError(429, 'too_many_attempts');
          if (typeof input.password !== 'string' || !input.password || input.password.length > 256) throw new HttpError(400, 'current_password_required');
          if (mfaRoutes?.enabled?.(actor) === true) {
            if ((input.code === undefined) === (input.recoveryCode === undefined)) throw new HttpError(400, 'mfa_code_required');
            await mfaRoutes.manage('reauth', { currentPassword: input.password, ...(input.code !== undefined ? { code: input.code } : { recoveryCode: input.recoveryCode }) }, { user, hash: owner, purpose: 'admin', admin: true });
          } else {
            if (input.code !== undefined || input.recoveryCode !== undefined) throw new HttpError(400, 'unexpected_field');
            const before = store.findUserById(actor);
            const valid = !!before && await passwordJob(() => verifyPassword(input.password, before.password_hash));
            const after = store.findUserById(actor);
            if (!valid || !after || after.password_hash !== before.password_hash) { store.audit('backup_reauthentication_failed', actor, ''); throw new HttpError(403, 'current_password_incorrect'); }
          }
          input.password = ''; currentAdmin();
        };
        if (path === '/api/v1/admin/backup') {
          if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
          const status = await backup.status({ owner, restricted: setup }); principal();
          send(200, { ...status, reauthentication: setup ? { setupCode: true, mfa: false } : { setupCode: false, mfa: mfaRoutes?.enabled?.(actor) === true }, ...(relay?.sessions && !setup ? { connections: connectionCounts(relay) } : {}) }, false); return true;
        }
        const download = /^\/api\/v1\/admin\/backup\/export\/([A-Za-z0-9_-]{32})$/.exec(path);
        if (path === '/api/v1/admin/backup/export' || download) {
          if (setup) throw new HttpError(428, 'credentials_change_required');
          if (download) {
            if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
            const entry = backup.download(download[1]); currentAdmin();
            store.audit('backup_downloaded', actor, String(entry.size));
            stream(entry); return true;
          }
          if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
          const input = await body(req, 4096);
          try {
            fields(input, ['password', 'code', 'recoveryCode', 'passphrase', 'passphraseConfirmation']); currentAdmin();
            if (typeof input.passphrase !== 'string' || input.passphrase !== input.passphraseConfirmation) throw new HttpError(400, 'backup_passphrase_mismatch');
            if (!backup.limits.exports.take(actor)) throw new HttpError(429, 'backup_rate_limited');
            await reauthenticate(input);
            const result = await backup.export({ passphrase: input.passphrase, actor, actorName: user.username });
            currentAdmin(); send(201, { download: result }, false); return true;
          } finally { wipe(input); }
        }
        const chunk = /^\/api\/v1\/admin\/backup\/restore\/upload\/([A-Za-z0-9_-]{32})$/.exec(path);
        if (path === '/api/v1/admin/backup/restore/upload') {
          if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
          const input = await body(req, 512);
          try {
            fields(input, setup ? ['size', 'setupCode'] : ['size']); principal();
            if (setup) proveSetup(input);
            const result = await backup.beginUpload({ size: input.size, owner, actor }); principal();
            send(201, result, false); return true;
          } finally { wipe(input); }
        }
        if (chunk) {
          if (req.method === 'DELETE') { principal(); send(200, await backup.cancelUpload({ uploadId: chunk[1], owner }), false); return true; }
          if (req.method !== 'PUT') throw new HttpError(405, 'method_not_allowed');
          const input = await body(req, 300000); fields(input, ['offset', 'data']); principal();
          send(200, await backup.appendUpload({ uploadId: chunk[1], owner, offset: input.offset, data: input.data }), false); return true;
        }
        if (path === '/api/v1/admin/backup/restore/inspect' || path === '/api/v1/admin/backup/restore/apply') {
          if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
          const applying = path.endsWith('/apply'), input = await body(req, 4096);
          try {
            fields(input, applying ? (setup ? ['uploadId', 'confirm', 'restoreAccess', 'setupCode'] : ['uploadId', 'confirm', 'restoreAccess', 'password', 'code', 'recoveryCode']) : (setup ? ['uploadId', 'passphrase', 'setupCode'] : ['uploadId', 'passphrase']));
            principal();
            if (typeof input.uploadId !== 'string' || !/^[A-Za-z0-9_-]{32}$/.test(input.uploadId)) throw new HttpError(400, 'backup_upload_not_found');
            if (applying && (input.confirm !== true || (input.restoreAccess !== undefined && typeof input.restoreAccess !== 'boolean'))) throw new HttpError(400, 'restore_confirmation_required');
            // The first setup's account is known to everyone: only who holds the
            // setup code may spend its budgets. A password guess spends them first.
            if (setup) proveSetup(input);
            if (!(applying ? backup.limits.applies : backup.limits.inspections).take(actor)) throw new HttpError(429, 'backup_rate_limited');
            if (!setup && applying) await reauthenticate(input);
            const result = applying ? await backup.apply({ uploadId: input.uploadId, owner, restoreAccess: input.restoreAccess === true, actor, actorName: user.username })
              : await backup.inspect({ uploadId: input.uploadId, owner, passphrase: input.passphrase, actor });
            // After an accepted restore this session may already be on its way out.
            if (!applying) principal();
            send(applying ? 202 : 200, result, false); return true;
          } finally { wipe(input); }
        }
        throw new HttpError(404, 'not_found');
      }
      if (user.must_change_credentials) throw new HttpError(428, 'credentials_change_required');
      if (path === '/api/v1/admin/server-access' || path === '/api/v1/admin/server-access/test' || path === '/api/v1/admin/server-access/save' || path === '/api/v1/admin/server-access/cancel') {
        if (!accessRuntime) throw new HttpError(503, 'server_access_unavailable');
        const context = { managementOrigin: publicOrigin };
        let result;
        if (path === '/api/v1/admin/server-access') {
          if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
          result = await accessRuntime.status({ ...context, listener: serverInfo.listener, deploymentKind: serverInfo.deployment?.kind, udpPort: serverInfo.udpPort ?? null });
          // How many people a save would cut off right now (counts only).
          if (relay?.sessions) result = { ...result, connections: connectionCounts(relay) };
        } else if (path.endsWith('/cancel')) {
          if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
          const input = await body(req, 1024); fields(input, ['operationId']);
          currentAdmin();
          if (typeof input.operationId !== 'string' || !/^[a-f0-9]{8}(?:-[a-f0-9]{4}){3}-[a-f0-9]{12}$/.test(input.operationId)) throw new HttpError(400, 'server_access_command_invalid');
          result = await accessRuntime.cancel(input.operationId);
          store.audit('server_access_cancelled', actor, input.operationId);
        } else {
          if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
          const saving = path.endsWith('/save'), input = await body(req, 16384);
          fields(input, saving ? ['config', 'testId', 'baseRevision', 'confirm'] : ['config', 'baseRevision']);
          currentAdmin();
          if (saving && input.confirm !== true) throw new HttpError(400, 'server_access_confirmation_required');
          // The trusted origin comes from listener admission, never the body.
          result = await (saving ? accessRuntime.save(input, context, { actor }) : accessRuntime.test(input, context));
          store.audit(saving ? 'server_access_saved' : 'server_access_tested', actor, saving ? result.operationId ?? '' : '');
        }
        currentAdmin(); send(path.endsWith('/save') ? 202 : 200, result); return true;
      }
      if (path === '/api/v1/admin/server-info') {
        if (req.method !== 'GET') throw new HttpError(405, 'server_update_not_available');
        let update=serverInfo.update;
        if(serverUpdate){try{update=await (serverUpdate.discover?serverUpdate.discover({authorize:currentAdmin}):serverUpdate.status());}catch{update={state:'unavailable',feedConfigured:true,applySupported:false,errorCode:'server_update_unavailable'};}}
        currentAdmin();
        // tls-expiry-v1: the presented certificate's expiry, as last checked (certificate-expiry.mjs).
        send(200, { ...serverInfo, update, tlsCertificate: certificates?.status() ?? { state: 'not_applicable' }, checkedAt: new Date().toISOString() }); return true;
      }
      if (path === '/api/v1/admin/server-update/check' || path === '/api/v1/admin/server-update/apply') {
        if(req.method!=='POST')throw new HttpError(405,'method_not_allowed');
        if(!serverUpdate)throw new HttpError(503,'server_update_unavailable');
        if(!store.onboarding(user).complete)throw new HttpError(428,'proxmox_setup_required');
        const input=await body(req,1024),apply=path.endsWith('/apply');fields(input,apply?['releaseId','fromVersion','confirm']:[]);
        if(apply&&(input.confirm!==true||typeof input.releaseId!=='string'||!/^[a-f0-9]{64}$/.test(input.releaseId)||input.fromVersion!==serverInfo.version))throw new HttpError(400,'update_confirmation_required');
        const current=currentAdmin();
        if(!store.onboarding(current).complete)throw new HttpError(428,'proxmox_setup_required');
        if(apply)accessRuntime?.assertAdmission();
        const result=apply?await serverUpdate.apply({releaseId:input.releaseId,fromVersion:input.fromVersion}):await serverUpdate.check();
        store.audit(apply?'server_update_requested':'server_update_checked',actor,apply?input.releaseId:'');send(202,result);return true;
      }
      if (path === '/api/v1/admin/inventory' && req.method === 'GET') {
        const inventory = store.inventory();
        const registered = store.registeredHostIds();
        inventory.desktops = inventory.desktops.filter(d => registered.has(d.id));
        if (relay) inventory.desktops = relay.decorate(inventory.desktops);
        const ids = new Set(inventory.desktops.map(d => d.id)); inventory.assignments = inventory.assignments.filter(a => ids.has(a.desktop_id));
        const hostUpdates = relay?.hostUpdate ? hostUpdateSummary(await relay.hostUpdate.views([...ids])) : undefined;
        send(200, { ...inventory, ...(hostUpdates ? { hostUpdates } : {}), ...(relay?.previews ? { desktopPreviews: { enabled: relay.previews.enabled() } } : {}), proxmox: metadata(), onboarding: store.onboarding(user), preservedUnavailableAssignments: inventory.users.reduce((sum, account) => sum + account.unavailableAssignmentCount, 0), audit: store.auditLog() }); return true;
      }
      if (store.onboarding(user).proxmoxRequired && !['/api/v1/admin/proxmox', '/api/v1/admin/proxmox/status', '/api/v1/admin/proxmox/sync', '/api/v1/admin/proxmox/vms'].includes(path)) throw new HttpError(428, 'proxmox_setup_required');
      // Read-only monitoring views of the console (admin-monitoring.mjs). The
      // console polls them, so they do not wake client desktop subscribers.
      if (path === '/api/v1/admin/sessions' || path === '/api/v1/admin/hosts' || path === '/api/v1/admin/stream-stats') {
        if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
        send(200, path.endsWith('/sessions') ? activeSessions(store, relay) : path.endsWith('/hosts') ? enrolledHosts(store, relay) : streamStatsSessions(store), false); return true;
      }
      // 홈 자원 (admin-resources.mjs), read-only: the server process, its data
      // directory and what the running desktops use. It may let the shared
      // Proxmox read run (at most every 10 s, waited for at most 3 s).
      if (path === '/api/v1/admin/resources') {
        if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
        if (!resources) throw new HttpError(503, 'resources_unavailable');
        const result = await resources.read(); currentAdmin();
        send(200, result, false); return true;
      }
      // 모니터링 (usage.mjs), read-only: who is signed in and connected now, the
      // sign-ins and connections of a range of days, and each account's and
      // desktop's connected time. Dates are the broker's local days, in the path
      // (queries are not accepted): /usage/live, /usage/summary/<from>/<to>,
      // /usage/events/<from>/<to>[/user/<login id>|/desktop/<id>][/page/<n>].
      if (path.startsWith('/api/v1/admin/usage/')) {
        if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
        const summary = /^\/api\/v1\/admin\/usage\/summary\/(\d{4}-\d\d-\d\d)\/(\d{4}-\d\d-\d\d)$/.exec(path);
        const events = /^\/api\/v1\/admin\/usage\/events\/(\d{4}-\d\d-\d\d)\/(\d{4}-\d\d-\d\d)(?:\/(user|desktop)\/([^/]{1,400}))?(?:\/page\/([1-9]\d{0,3}))?$/.exec(path);
        let result = null;
        if (path === '/api/v1/admin/usage/live') result = usageLive(store, relay);
        else if (summary) result = await usageSummaryShared(store, relay, { from: summary[1], to: summary[2] });
        else if (events) {
          let subject = null;
          if (events[3]) { try { subject = decodeURIComponent(events[4]); } catch { throw new HttpError(400, 'invalid_usage_range'); } }
          if (subject !== null && (subject.length > 128 || /[\x00-\x1f]/.test(subject))) throw new HttpError(400, 'invalid_usage_range');
          result = usageEvents(store, relay, { from: events[1], to: events[2], account: events[3] === 'user' ? subject : null, desktopId: events[3] === 'desktop' ? subject : null, page: events[5] ? Number(events[5]) : 1 });
        } else throw new HttpError(404, 'not_found');
        if (!result) throw new HttpError(400, 'invalid_usage_range');
        send(200, result, false); return true;
      }
      // 공지 보내기 (notices.mjs), administrators only like everything here:
      // GET /notices (who is signed in or connected now, the limits and the sent
      // history), POST /notices (send), POST /notices/<id>/withdraw (stop showing).
      // The read is polled by the console and does not wake client subscribers.
      const noticeWithdraw = /^\/api\/v1\/admin\/notices\/([a-f0-9]{32})\/withdraw$/.exec(path);
      if (path === '/api/v1/admin/notices' || noticeWithdraw) {
        if (!notices) throw new HttpError(503, 'notices_unavailable');
        if (path === '/api/v1/admin/notices' && req.method === 'GET') { send(200, notices.adminList(), false); return true; }
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        const input = await body(req, 4096);
        const current = currentAdmin();
        if (noticeWithdraw) { fields(input, []); send(200, notices.withdraw(noticeWithdraw[1], current, currentAdmin), false); return true; }
        send(201, notices.send(input, current, currentAdmin, requestLanguage(req.headers['accept-language'])), false); return true;
      }
      // desktop-preview-v1 off switch (미리보기 화면 끄기); on unless turned off.
      // Turning it off forgets every picture; clients re-read their list.
      if (path === '/api/v1/admin/desktop-previews') {
        if (req.method !== 'PUT') throw new HttpError(405, 'method_not_allowed');
        if (!relay?.previews) throw new HttpError(503, 'desktop_previews_unavailable');
        const input = await body(req, 256); fields(input, ['enabled']);
        if (typeof input.enabled !== 'boolean') throw new HttpError(400, 'invalid_desktop_previews');
        const current = currentAdmin();
        send(200, relay.previews.setEnabled(input.enabled, current.id)); return true;
      }
      if (path === '/api/v1/admin/hosts/requests' && req.method === 'GET') {
        if (!vault.verifyCsrf(token, req.headers['x-csrf-token'])) throw new HttpError(403, 'csrf_required');
        // Requests from VMs without an earlier Host are placed by a bounded,
        // rate-limited Proxmox read (host-vm-lookup.mjs); an older test double
        // without it keeps the plain list.
        const listing = typeof hostRequests.pendingResolved === 'function' ? await hostRequests.pendingResolved() : hostRequests.pending();
        currentAdmin();
        send(200, listing); return true;
      }
      const hostRemoval = /^\/api\/v1\/admin\/desktops\/([A-Za-z0-9_-]+)\/host$/.exec(path);
      if (hostRemoval) {
        if (req.method !== 'DELETE') throw new HttpError(405, 'method_not_allowed');
        const input = await body(req, 1024); fields(input, ['expectedHostId', 'confirm']); currentAdmin();
        if (input.expectedHostId === undefined) throw new HttpError(428, 'host_identity_required');
        if (typeof input.expectedHostId !== 'string' || !/^[A-Za-z0-9_-]{1,128}$/.test(input.expectedHostId)) throw new HttpError(400, 'invalid_host_identity');
        if (input.confirm !== true) throw new HttpError(400, 'host_removal_confirmation_required');
        const result = store.removeHostRegistration(hostRemoval[1], input.expectedHostId, actor, () => {
          currentAdmin();
          let observation; try { observation = hostRemovalActivity?.(hostRemoval[1]); } catch { }
          const groups = { sessions: ['total','stream','attached','tickets'], channels: ['clipboard','clipboardTransfers','drive','driveShares','driveRequests','usb','usbDevices','print','printTransfers'], operations: ['display','console','vmConfig','vmConfigUncertain','sessionCleanup','hostUpdates','power','serverAccess'] };
          if (observation?.protocol !== 'activity-desktop-v1' || observation.complete !== true || !Number.isSafeInteger(observation.observedAtMs) || Math.abs(Date.now() - observation.observedAtMs) > 1000) throw new HttpError(503, 'host_activity_unavailable');
          let busy = false;
          for (const [name, keys] of Object.entries(groups)) {
            const group = observation[name];
            if (!group || typeof group !== 'object' || Array.isArray(group) || Object.keys(group).length !== keys.length || keys.some(key => !Object.hasOwn(group,key) || !Number.isSafeInteger(group[key]) || group[key] < 0 || group[key] > 1000000)) throw new HttpError(503, 'host_activity_unavailable');
            if (keys.some(key => group[key] !== 0)) busy = true;
          }
          if (busy) throw new HttpError(409, 'host_registration_busy');
        });
        // Normal send() revalidates the now-revoked idle channel and pushes the
        // list change. The VM record stays; this desktop's assignments went in
        // the same transaction (result.removedAssignments).
        send(200, { ok: true, ...result }); return true;
      }
      const hostRequestMatch = /^\/api\/v1\/admin\/hosts\/requests\/([0-9a-f-]{36})\/(approve|approve-pc|reject)$/.exec(path);
      if (hostRequestMatch && req.method === 'POST') {
        const input = await body(req, 1024);
        const authorize = () => {
          const current = currentAdmin();
          if (!store.onboarding(current).complete) throw new HttpError(428, 'proxmox_setup_required');
          return current.id;
        };
        if (hostRequestMatch[2] === 'approve') send(200, await hostRequests.approve(hostRequestMatch[1], input, authorize));
        else if (hostRequestMatch[2] === 'approve-pc') send(200, await hostRequests.approveLocalPc(hostRequestMatch[1], input, authorize));
        else { fields(input, []); send(200, hostRequests.reject(hostRequestMatch[1], authorize)); }
        return true;
      }
      // Compatibility: clipboardPolicy/devicePolicy on account create or
      // update are validated and applied to every current assignment of that
      // account (none for a new account). Per-VM values use assignments/batch.
      if (path === '/api/v1/admin/users' && req.method === 'POST') {
        const input = await body(req); fields(input, ['username', 'notes', 'displayName', 'password', 'passwordConfirmation', 'role', 'clipboardPolicy', 'devicePolicy']);
        currentAdmin();
        const passwordHash = await passwordJob(() => hashPassword(confirmedPassword(input))); input.password = ''; input.passwordConfirmation = '';
        currentAdmin();
        store.addUser({ name: input.username, notes: input.notes, displayName: input.displayName, passwordHash, role: input.role, clipboardPolicy: input.clipboardPolicy, devicePolicy: input.devicePolicy }, actor);
        send(201, { ok: true }); return true;
      }
      if (path === '/api/v1/admin/users' && req.method === 'GET') { send(200, { users: store.inventory().users }); return true; }
      const userMatch = /^\/api\/v1\/admin\/users\/([^/]+)(\/password|\/enabled)?$/.exec(path);
      if (userMatch) {
        let name; try { name = decodeURIComponent(userMatch[1]); } catch { throw new HttpError(400, 'invalid_username'); }
        if (req.method === 'GET' && !userMatch[2]) {
          const account = store.findUser(name); if (!account) throw new HttpError(404, 'user_not_found');
          send(200, { user: { ...publicUser(account), disabled: !!account.disabled, assignments: store.userAssignments(account.id) } }); return true;
        }
        if (req.method === 'DELETE' && !userMatch[2]) {
          if ((req.headers['content-type'] ?? '').split(';')[0].trim() !== 'application/json') throw new HttpError(428, 'account_identity_required');
          const input = await body(req, 1024); currentAdmin(); fields(input, ['expectedAccountId']);
          store.deleteUser(name, actor, expectedAccountId(input)); send(200, { ok: true }); return true;
        }
        if (req.method === 'PUT') {
          const input = await body(req);
          currentAdmin();
          const expected = expectedAccountId(input);
          if (userMatch[2] === '/password') { fields(input, ['expectedAccountId', 'password', 'passwordConfirmation']); const target = store.findUser(name); if (target?.is_primary_admin && target.id !== actor) { input.password = ''; input.passwordConfirmation = ''; throw new HttpError(403, 'primary_admin_protected'); } const hashed = await passwordJob(() => hashPassword(confirmedPassword(input))); input.password = ''; input.passwordConfirmation = ''; currentAdmin(); store.changePassword(name, hashed, actor, expected); }
          else if (userMatch[2] === '/enabled') { fields(input, ['expectedAccountId', 'enabled']); if (typeof input.enabled !== 'boolean') throw new HttpError(400, 'invalid_enabled'); store.setUserEnabled(name, input.enabled, actor, expected); }
          else { fields(input, ['expectedAccountId', 'notes', 'displayName', 'role', 'clipboardPolicy', 'devicePolicy']); store.updateUser(name, input, actor, expected); relay?.clipboardPolicyChanged(expected); relay?.devicePolicyChanged(expected); }
          send(200, { ok: true }); return true;
        }
      }
      if (path === '/api/v1/admin/desktops' && req.method === 'POST') {
        throw new HttpError(405, 'proxmox_import_required');
      }
      if (path === '/api/v1/admin/host-updates' || path === '/api/v1/admin/host-updates/all' || /^\/api\/v1\/admin\/desktops\/[A-Za-z0-9_-]+\/host-update$/.test(path)) {
        if (!relay?.hostUpdate) throw new HttpError(503, 'host_update_unavailable');
        const managedIds = () => [...store.registeredHostIds()];
        if (path === '/api/v1/admin/host-updates') {
          if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
          send(200, hostUpdateSummary(await relay.hostUpdate.views(managedIds()))); return true;
        }
        if (req.method !== 'POST') throw new HttpError(405, 'method_not_allowed');
        const input = await body(req, 1024);
        // The same administrator session must still be current after the body.
        currentAdmin();
        const authorize = () => {
          currentAdmin();
          return {};
        };
        if (path === '/api/v1/admin/host-updates/all') {
          fields(input, ['version']);
          const results = [];
          for (const id of managedIds()) {
            const view = (await relay.hostUpdate.views([id])).desktops[id];
            if (!view.supported || !view.newer || view.latestVersion !== input.version || view.operation && ['waiting', 'dispatched', 'accepted', 'downloading', 'verifying', 'installing'].includes(view.operation.status)) { results.push({ desktopId: id, started: false, reason: !view.connected ? 'host_offline' : !view.supported ? 'host_update_unsupported' : !view.newer ? 'host_update_current' : view.latestVersion !== input.version ? 'host_release_unavailable' : 'host_update_in_progress' }); continue; }
            try { const updated = await relay.hostUpdate.request({ actor, desktopId: id, version: input.version, disconnectSessions: true, source: 'admin', authorize }); results.push({ desktopId: id, started: true, status: updated.operation?.status ?? null }); }
            catch (error) { if (!(error instanceof HttpError)) throw error; results.push({ desktopId: id, started: false, reason: error.code }); }
          }
          send(202, { version: input.version, results }); return true;
        }
        const id = /^\/api\/v1\/admin\/desktops\/([A-Za-z0-9_-]+)\/host-update$/.exec(path)[1];
        managedHostDesktop(id); fields(input, ['version', 'disconnectSessions', 'allowDowngrade']);
        send(202, { hostUpdate: await relay.hostUpdate.request({ actor, desktopId: id, version: input.version, disconnectSessions: input.disconnectSessions ?? false, allowDowngrade: input.allowDowngrade ?? false, source: 'admin', authorize }) }); return true;
      }
      if (path === '/api/v1/admin/client-diagnostics') {
        if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
        send(200, clientDiagnostics(store)); return true;
      }
      const desktopClientDiagnostics = /^\/api\/v1\/admin\/desktops\/([A-Za-z0-9_-]+)\/client-diagnostics$/.exec(path);
      if (desktopClientDiagnostics) {
        managedHostDesktop(desktopClientDiagnostics[1]);
        if (req.method !== 'GET') throw new HttpError(405, 'method_not_allowed');
        send(200, clientDiagnostics(store, desktopClientDiagnostics[1])); return true;
      }
      const desktopDiagnostics = /^\/api\/v1\/admin\/desktops\/([A-Za-z0-9_-]+)\/diagnostics$/.exec(path);
      if (desktopDiagnostics) {
        managedHostDesktop(desktopDiagnostics[1]);
        if (req.method === 'GET') { send(200, relay.diagnosticStatus(desktopDiagnostics[1])); return true; }
        if (req.method === 'POST') { fields(await body(req), []); currentAdmin(); send(202, relay.requestDiagnostics(desktopDiagnostics[1])); return true; }
        throw new HttpError(405, 'method_not_allowed');
      }
      const desktopMatch = /^\/api\/v1\/admin\/desktops\/([A-Za-z0-9_-]+)(\/enabled|\/power|\/enrollment)?$/.exec(path);
      if (desktopMatch) {
        const id = desktopMatch[1];
        // Physical PC names are presentation only. Registration and Host
        // replacement remain exclusively in the code-verified approval flow.
        if (req.method === 'PUT' && !desktopMatch[2] && store.inventory().desktops.some(d => d.id === id && d.kind === 'pc' && d.localPcApproved === true)) {
          const input = await body(req); fields(input, ['name']); currentAdmin();
          if (!store.inventory().desktops.some(d => d.id === id && d.localPcApproved === true)) throw new HttpError(404, 'desktop_not_found');
          store.updateDesktop(id, input, actor); send(200, { ok: true }); return true;
        }
        managedVm(id);
        if (req.method === 'POST' && desktopMatch[2] === '/enrollment') {
          const input = await body(req); fields(input, []);
          currentAdmin();
          send(201, await relay.createEnrollment(managedVm(id), actor, currentAdmin)); return true;
        }
        if (req.method === 'DELETE' && !desktopMatch[2]) throw new HttpError(405, 'proxmox_managed_vm_cannot_delete');
        if (req.method === 'PUT') {
          const input = await body(req);
          currentAdmin();
          if (desktopMatch[2] === '/enabled') throw new HttpError(405, 'vm_provided_status_removed');
          else if (!desktopMatch[2]) { fields(input, ['name', 'description', 'os']); store.updateDesktop(id, input, actor); }
          else throw new HttpError(405, 'method_not_allowed');
          send(200, { ok: true }); return true;
        }
        if (req.method === 'POST' && desktopMatch[2] === '/power') {
          const input = await body(req); fields(input, ['action']);
          currentAdmin();
          if (!['start', 'shutdown', 'reboot', 'stop'].includes(input.action)) throw new HttpError(400, 'invalid_power_action');
          const desktop = store.db.prepare('SELECT * FROM desktops WHERE id=? AND pve_available=1').get(id);
          if (!desktop?.pve_vmid) throw new HttpError(409, 'desktop_not_linked');
          const authorize = () => {
            currentAdmin();
            const vm = managedVm(id); if (!vm.pve_available) throw new HttpError(409, 'desktop_not_linked'); return vm;
          };
          const result = await power.submit(authorize, actor, input.action, { legacyAdmin: true });
          send(202, result); return true;
        }
      }
      if (path === '/api/v1/admin/assignments/batch' && req.method === 'PUT') {
        // One atomic update of a user's VMs and each VM's own permissions:
        // assignments: [{ desktopId, clipboardPolicy?, devicePolicy?, soundCardAdd? }]
        // (omitted policy: keep an existing assignment's / deny a new one), or
        // the legacy desktopIds: [id] that keeps existing permissions.
        const input = await body(req, 262144); fields(input, ['username', 'expectedAccountId', 'desktopIds', 'assignments']);
        if ((input.desktopIds === undefined) === (input.assignments === undefined)) throw new HttpError(400, 'invalid_desktop_ids');
        if (input.desktopIds !== undefined && (!Array.isArray(input.desktopIds) || input.desktopIds.some(id => typeof id !== 'string'))) throw new HttpError(400, 'invalid_desktop_ids');
        if (input.assignments !== undefined && (!Array.isArray(input.assignments) || input.assignments.some(entry => !entry || typeof entry !== 'object' || Array.isArray(entry)))) throw new HttpError(400, 'invalid_assignment_entry');
        if (input.assignments?.some(entry => entry.soundCardAdd !== undefined && typeof entry.soundCardAdd !== 'boolean')) throw new HttpError(400, 'invalid_sound_card_add');
        currentAdmin();
        const expected = expectedAccountId(input);
        const result = store.replaceVmAssignments(input.username, input.assignments ?? input.desktopIds, actor, expected, { registeredOnly: true });
        // Only sessions whose own VM grant was restricted end; removed
        // assignments end through the authorization sweep as before.
        const changed = result.policiesChanged ? expected : undefined;
        if (changed) { relay?.clipboardPolicyChanged(changed); relay?.devicePolicyChanged(changed); }
        send(200, { ok: true, ...result }); return true;
      }
      const desktopPermissions = /^\/api\/v1\/admin\/desktops\/([A-Za-z0-9_-]+)\/permissions$/.exec(path);
      if (desktopPermissions) {
        // One atomic update of several accounts' permissions on this one VM:
        // permissions: [{ username, clipboardPolicy?, devicePolicy?, soundCardAdd? }].
        // Every account must already have the VM; nothing is assigned or removed.
        if (req.method !== 'PUT') throw new HttpError(405, 'method_not_allowed');
        const input = await body(req, 262144); fields(input, ['permissions']);
        currentAdmin();
        const id = managedHostDesktop(desktopPermissions[1]).id;
        if (!Array.isArray(input.permissions) || !input.permissions.length || input.permissions.length > 500) throw new HttpError(400, 'invalid_permission_entries');
        for (const entry of input.permissions) {
          if (!entry || typeof entry !== 'object' || Array.isArray(entry)) throw new HttpError(400, 'invalid_assignment_entry');
          expectedAccountId(entry);
        }
        const result = store.replaceDesktopPermissions(id, input.permissions, actor);
        for (const userId of result.changedUserIds) { relay?.clipboardPolicyChanged(userId, id); relay?.devicePolicyChanged(userId, id); }
        send(200, { ok: true, count: result.count, policiesChanged: result.changedUserIds.length }); return true;
      }
      if (path === '/api/v1/admin/assignments' && ['POST', 'DELETE'].includes(req.method)) {
        const input = await body(req); fields(input, ['username', 'expectedAccountId', 'desktopId']);
        currentAdmin();
        managedVm(input.desktopId);
        const expected = expectedAccountId(input);
        if (req.method === 'POST') store.assign(input.username, input.desktopId, actor, expected); else store.revoke(input.username, input.desktopId, actor, expected);
        send(200, { ok: true }); return true;
      }
      if (path === '/api/v1/admin/proxmox/console-permission' && req.method === 'POST') {
        fields(await body(req, 256), []); currentAdmin();
        const revision = store.setting('proxmox');
        if (!revision) throw new HttpError(409, 'proxmox_not_configured');
        const permissions = await proxmox.call(requireConfig(), '/access/permissions?path=%2Fvms');
        currentAdmin();
        if (store.setting('proxmox') !== revision) throw new HttpError(409, 'proxmox_settings_changed');
        const granted = vmsPrivilege(permissions, 'VM.Console');
        consoleAccess.record(granted);
        send(200, { granted, scope: '/vms', checkedAt: new Date().toISOString(), proxmox: metadata() }, false); return true;
      }
      if (path === '/api/v1/admin/proxmox' && req.method === 'PUT') {
        const input = await body(req, 24576); fields(input, ['endpoint', 'tokenId', 'secret', 'ca', 'fingerprint', 'expiresAt']);
        const { validateProxmox } = await import('./proxmox.mjs');
        currentAdmin();
        const previous = config();
        const value = validateProxmox(input, previous);
        if (previous && value.endpoint !== previous.endpoint && store.db.prepare('SELECT 1 FROM desktops WHERE pve_vmid IS NOT NULL LIMIT 1').get()) throw new HttpError(409, 'proxmox_has_linked_desktops');
        store.saveSetting('proxmox', vault.seal(value), actor); input.secret = ''; agentAccess.reset(); consoleAccess.record(null);
        store.saveSetting('proxmox_sync', JSON.stringify({ state: 'unchecked' }), actor);
        store.saveSetting('proxmox_token_status', '{}', actor);
        send(200, { proxmox: metadata(), onboarding: store.onboarding(user) }); return true;
      }
      if (path === '/api/v1/admin/proxmox/status' && req.method === 'GET') {
        const revision = store.setting('proxmox'), current = requireConfig();
        let status;
        try {
          if (typeof proxmox.tokenStatus !== 'function') throw new HttpError(502, 'proxmox_expiry_unavailable');
          status = { ...await proxmox.tokenStatus(current), expirySource: 'proxmox', expiryCheckedAt: new Date().toISOString() };
        } catch (error) {
          status = { expiryCheckedAt: new Date().toISOString(), expiryError: error instanceof HttpError ? error.code : 'proxmox_expiry_unavailable' };
        }
        if (store.setting('proxmox') !== revision) throw new HttpError(409, 'proxmox_settings_changed');
        store.saveSetting('proxmox_token_status', JSON.stringify(status), actor);
        send(200, { proxmox: metadata(), onboarding: store.onboarding(user) }); return true;
      }
      if (path === '/api/v1/admin/proxmox/vms' && req.method === 'GET') {
        currentAdmin(); const revision = store.setting('proxmox'), current = requireConfig();
        // A registered VM that moved (migration, HA, restore to a new VM ID) is looked up by its UUID first (vm-relocate.mjs).
        const { targets, vms } = await readRegisteredInventory({ proxmox, relocation, store, config: current, revision, targets: store.registeredVmTargets(), waitMs: 8000 });
        currentAdmin();
        if (store.setting('proxmox') !== revision) throw new HttpError(409, 'proxmox_settings_changed');
        if (JSON.stringify(store.registeredVmTargets()) !== JSON.stringify(targets)) throw new HttpError(409, 'host_registration_changed');
        const allowed = new Set(targets.map(target => target.node + ':' + target.vmid));
        if (!Array.isArray(vms) || vms.length !== targets.length || new Set(vms.map(vm => vm?.node + ':' + vm?.vmid)).size !== targets.length || vms.some(vm => !allowed.has(vm?.node + ':' + vm?.vmid))) throw new HttpError(502, 'proxmox_invalid_response');
        send(200, { vms, checkedAt: new Date().toISOString() }); return true;
      }
      if (path === '/api/v1/admin/proxmox/import' && req.method === 'POST') {
        throw new HttpError(405, 'proxmox_sync_required');
      }
      if (path === '/api/v1/admin/proxmox/sync' && req.method === 'POST') {
        const input = await body(req); fields(input, []);
        currentAdmin();
        const revision = store.setting('proxmox');
        const current = requireConfig();
        const sequence = (syncSequences.get(store) ?? 0) + 1; syncSequences.set(store, sequence);
        try {
          let targets = store.registeredVmTargets(), relocations = [];
          let permissions, vms;
          if (!targets.length) {
            if (typeof proxmox.call !== 'function') throw new HttpError(503, 'proxmox_validation_unavailable');
            permissions = await proxmox.call(current, '/access/permissions?path=%2Fvms');
            if (!permissions || typeof permissions !== 'object' || Array.isArray(permissions)) throw new HttpError(502, 'proxmox_invalid_response');
            vms = [];
          } else {
            ({ targets, vms, relocations } = await readRegisteredInventory({ proxmox, relocation, store, config: current, revision, targets, force: true, waitMs: 8000 }));
            try { permissions = await proxmox.call(current, '/access/permissions?path=%2Fvms'); } catch { /* Existing scoped observations remain useful without optional privilege metadata. */ }
          }
          currentAdmin();
          if (store.setting('proxmox') !== revision) throw new HttpError(409, 'proxmox_settings_changed');
          if (syncSequences.get(store) !== sequence) throw new HttpError(409, 'proxmox_sync_superseded');
          if (JSON.stringify(store.registeredVmTargets()) !== JSON.stringify(targets)) throw new HttpError(409, 'host_registration_changed');
          // This callback receives only observed registered rows, never a
          // cluster discovery result. Complete all awaits before final commit.
          try { await audioInventory?.(current, vms); } catch { /* Optional audio metadata does not change registration. */ }
          let result;
          try { result = store.refreshRegisteredVms(targets, vms, actor, () => {
            currentAdmin();
            if (store.setting('proxmox') !== revision) throw new HttpError(409, 'proxmox_settings_changed');
            if (syncSequences.get(store) !== sequence) throw new HttpError(409, 'proxmox_sync_superseded');
          }); }
          catch (error) { if (error.message === 'host_registration_changed') throw new HttpError(409, error.message); if (/^invalid_/.test(error.message)) throw new HttpError(502, 'proxmox_invalid_response'); throw error; }
          if (permissions) consoleAccess.record(vmsPrivilege(permissions, 'VM.Console'));
          // Every registered VM failed on the certificate: the connection itself
          // is refused (a pinned certificate was replaced, for one). The rows
          // stay as recorded above; the connection says why.
          if (vms.length && vms.every(vm => vm?.powerError === 'proxmox_certificate_error')) {
            store.saveSetting('proxmox_sync', JSON.stringify({ ...readStatus('proxmox_sync'), state: 'error', attemptedAt: new Date().toISOString(), errorCode: 'proxmox_certificate_error' }), actor);
          }
          // Registered VMs looked up by UUID in this refresh and what came of it.
          const relocated = relocations.map(({ desktopId, state, from, to }) => ({ desktopId, state, from, ...(to ? { to } : {}) }));
          send(200, { ...result, ...(relocated.length ? { relocations: relocated } : {}), proxmox: metadata(), onboarding: store.onboarding(user) }); return true;
        } catch (error) {
          currentAdmin();
          if (store.setting('proxmox') === revision && syncSequences.get(store) === sequence && !(error instanceof HttpError && error.status === 409)) {
            const previous = readStatus('proxmox_sync');
            store.saveSetting('proxmox_sync', JSON.stringify({ ...previous, state: 'error', attemptedAt: new Date().toISOString(), errorCode: error instanceof HttpError ? error.code : 'proxmox_invalid_response' }), actor);
          }
          throw error;
        }
      }
      if (path === '/api/v1/admin/proxmox/task' && req.method === 'POST') {
        const input = await body(req); fields(input, ['node', 'task']);
        currentAdmin();
        const result = await proxmox.task(requireConfig(), input.node, input.task); send(200, result); return true;
      }
      throw new HttpError(404, 'not_found');
    } catch (error) {
      if (error instanceof HttpError) throw error;
      if (/UNIQUE constraint failed/.test(error.message)) throw new HttpError(409, 'already_exists');
      if (error.message === 'primary_admin_protected') throw new HttpError(403, error.message);
      if (['primary_admin_required', 'last_admin_required', 'cannot_delete_self', 'user_or_desktop_unavailable', 'assignment_changed', 'account_identity_changed', 'host_registration_changed', 'host_not_registered', 'username_must_change', 'setup_not_required'].includes(error.message)) throw new HttpError(409, error.message);
      if (['user_not_found', 'desktop_not_found'].includes(error.message)) throw new HttpError(404, error.message);
      if (/^(invalid_|password_length_)/.test(error.message) || error.message === 'sound_card_vm_only') throw new HttpError(400, error.message);
      throw error;
    }
  };
}
