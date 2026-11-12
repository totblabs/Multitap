// 백업·복원 (server/src/backup.mjs): make a backup file, and put a server back
// from one. The same restore steps serve the console page and the first setup
// screen (app.js), which differ only in what proves who is asking: the
// administrator's password, or the one-time setup code.
import { el, button, icon, panel, facts, disable, date, bytes } from './dom.js';
import { messages } from './labels.js';
import { announceFirst, announcements, everyoneAudience } from './notices.js';
import { t } from './i18n.js';
import { commandBox } from '../proxmox-shell-setup.js';

// A Docker install restores with a command on the server (restore.state 'docker-command', server/src/backup.mjs):
// the server names the command, the console adds where the file goes. Only a plain command is shown.
export const dockerRestoreCommand = status => status?.restore?.state === 'docker-command' && typeof status.restore.command === 'string' && /^[A-Za-z0-9 ._/-]{1,80}$/.test(status.restore.command) ? status.restore.command : null;
// What a server that cannot restore from the console says: on Docker the command to run there.
function unsupportedNote(status) {
  const command = dockerRestoreCommand(status);
  if (!command) return el('p', { class: 'note' }, icon('info'), el('span', {}, t('backup.104')));
  return el('div', { class: 'backup-docker' },
    el('p', { class: 'note' }, icon('info'), el('span', {}, t('backup.dockerNote'))),
    commandBox(el, button, t('backup.dockerCommand', { command })),
    el('p', { class: 'field-hint backup-lines' }, t('backup.dockerHelp')));
}
export const validResponse = value => value?.protocol === 'backup-v1' && !!value.export && !!value.restore && typeof value.restore.supported === 'boolean';

// Why a restore did not happen, by the updater's fixed codes.
export const restoreErrors = {
  restore_idle_timeout: t('backup.1'),
  restore_fence_lost: t('backup.2'), restore_activity_changed: t('backup.3'),
  restore_connections_busy: t('backup.4'), restore_disk_space: t('backup.5'),
  restore_staging_changed: t('backup.6'), restore_staging_invalid: t('backup.7'), restore_staged_database_invalid: t('backup.8'),
  restore_installation_changed: t('backup.9'), restore_health_failed: t('backup.10'),
  restore_interrupted: t('backup.11'), restore_recovery_required: t('backup.12'),
};
export const blockerText = { restore_host_updates_busy: t('common.33'), restore_desktop_settings_busy: t('backup.13'), restore_power_busy: t('backup.14'), restore_activity_unverified: t('backup.15') };
const activeStates = new Set(['waiting_idle', 'applying', 'health_check']);
const stateText = { waiting_idle: t('backup.16'), applying: t('backup.17'), health_check: t('backup.18') };
const failureText = code => restoreErrors[code] ?? messages[code] ?? t('backup.19');

const uiOf = ctx => ctx.state.ui.backup ??= { restoreAccess: false };
function base64(bytes) {
  let text = '';
  for (let index = 0; index < bytes.length; index += 0x8000) text += String.fromCharCode.apply(null, bytes.subarray(index, index + 0x8000));
  return globalThis.btoa(text);
}
const sameFile = (a, b) => !!a && !!b && a.name === b.name && a.size === b.size && a.lastModified === b.lastModified;

// ---- making a backup file ------------------------------------------------
function exportBackup(ctx, status) {
  const ui = uiOf(ctx), mfa = status?.reauthentication?.mfa === true, minimum = status?.export?.passphraseMinimum ?? 12;
  ctx.modal(t('backup.20'), [
    { name: 'passphrase', label: t('backup.21'), type: 'password', min: minimum, max: 256, hint: t('backup.22', { minimum }) },
    { name: 'passphraseConfirmation', label: t('backup.23'), type: 'password', min: minimum, max: 256 },
    { name: 'password', label: t('backup.24'), type: 'password', max: 256, hint: t('backup.25') },
    ...(mfa ? [{ name: 'code', label: t('backup.26'), max: 6, min: 6, placeholder: t('backup.27') }] : []),
  ], async input => {
    if (input.passphrase !== input.passphraseConfirmation) throw new Error(messages.backup_passphrase_mismatch);
    const action = ctx.pageAction('backup-export');
    const result = await ctx.api('/backup/export', 'POST', { passphrase: input.passphrase, passphraseConfirmation: input.passphraseConfirmation, password: input.password, ...(mfa ? { code: input.code } : {}) });
    input.passphrase = ''; input.passphraseConfirmation = ''; input.password = '';
    action.check();
    ui.download = result.download;
    void ctx.load('backup', { force: true });
    ctx.notice(t('backup.28'));
    // A same-origin attachment: the browser saves it without leaving the page.
    try { const link = el('a', { href: downloadHref(result.download), download: result.download.filename, hidden: true }); globalThis.document.body.append(link); link.click?.(); link.remove(); } catch { /* The link on the page works as well. */ }
  }, { save: t('backup.29'), reload: false, description: t('backup.30') });
}
const downloadHref = download => '/api/v1/admin/backup/export/' + encodeURIComponent(download.id);

function exportPanel(ctx, status) {
  const ui = uiOf(ctx), last = status?.export?.last, fresh = ui.download && Date.parse(ui.download.expiresAt) > Date.now() ? ui.download : null;
  const make = disable(button(t('backup.20'), () => exportBackup(ctx, status), 'primary', 'download'), !status ? t('backup.31') : null);
  return panel({ title: t('backup.20'), className: 'backup-export' },
    el('p', { class: 'field-hint backup-lines' }, t('backup.32')),
    fresh ? el('div', { class: 'backup-download' }, el('div', {}, el('strong', {}, fresh.filename), el('small', {}, t('backup.33', { bytes: bytes(fresh.size) }))),
      el('a', { class: 'button secondary', href: downloadHref(fresh), download: fresh.filename }, icon('download'), t('backup.34'))) : null,
    last?.at ? facts([[t('backup.35'), date(last.at), [last.actor, Number.isFinite(last.size) ? bytes(last.size) : null].filter(Boolean).join(' · ') || null]]) : null,
    el('div', { class: 'backup-actions' }, make));
}

// ---- restoring -----------------------------------------------------------
// `flow` is where the steps live: { ctx, setup, render, fail(text), proof() }.
// proof() gives what the server asks of this place (the setup code, or nothing
// until the final confirmation asks for the password).
async function uploadAndInspect(flow, file, passphrase) {
  const { ctx } = flow, ui = uiOf(ctx), action = ctx.pageAction('backup-restore');
  ui.error = ''; ui.busy = 'upload'; ui.progress = 0; flow.render();
  try {
    // A second try with another passphrase reads the file the server already has.
    if (!(ui.uploadId && sameFile(ui.uploaded, file))) {
      const begun = await ctx.api('/backup/restore/upload', 'POST', { size: file.size, ...flow.proof() });
      action.check(); ui.uploadId = null; ui.uploaded = null;
      const step = begun.chunkBytes; let offset = 0, shown = 0;
      while (offset < file.size) {
        const part = new Uint8Array(await file.slice(offset, Math.min(file.size, offset + step)).arrayBuffer());
        const answer = await ctx.api('/backup/restore/upload/' + begun.uploadId, 'PUT', { offset, data: base64(part) });
        action.check(); offset = answer.received; ui.progress = offset / file.size;
        if (Date.now() - shown > 250) { shown = Date.now(); flow.render(); }
      }
      ui.uploadId = begun.uploadId; ui.uploaded = { name: file.name, size: file.size, lastModified: file.lastModified };
    }
    ui.busy = 'inspect'; flow.render();
    const result = await ctx.api('/backup/restore/inspect', 'POST', { uploadId: ui.uploadId, passphrase, ...flow.proof() });
    action.check();
    ui.preview = result.preview; ui.restoreAccess = false;
  } catch (error) {
    if (!action.current()) return;
    // Only a wrong passphrase keeps the upload; anything else starts over.
    if (error.code !== 'backup_unreadable' && error.code !== 'backup_busy') { ui.uploadId = null; ui.uploaded = null; }
    ui.error = error.message;
  } finally { if (action.current()) { ui.busy = null; flow.render(); } }
}
async function forget(flow) {
  const { ctx } = flow, ui = uiOf(ctx), id = ui.uploadId;
  ui.dismissed = id;
  ui.uploadId = null; ui.uploaded = null; ui.preview = null; ui.error = ''; ui.restoreAccess = false; flow.render();
  if (id) try { await ctx.api('/backup/restore/upload/' + id, 'DELETE'); } catch { /* It expires by itself. */ }
}

function confirmRestore(flow, status) {
  const { ctx } = flow, ui = uiOf(ctx), preview = ui.preview, connections = status?.connections;
  const people = connections && connections.sessions + connections.consoles > 0 ? t('backup.36', { people: connections.people }) : t('backup.37');
  const admins = preview.counts.adminNames.length ? t('backup.38', { join: preview.counts.adminNames.join(', ') }) : '';
  const mfa = status?.reauthentication?.mfa === true;
  // 먼저 알리기 (js/notices.js). Not during the first setup: nobody is signed in to a server that has no accounts yet.
  const announce = flow.setup ? null : announceFirst(ctx, { audience: everyoneAudience(ctx), text: announcements.restore });
  ctx.modal(t('backup.39'), flow.setup ? [] : [
    { name: 'password', label: t('backup.24'), type: 'password', max: 256, hint: t('backup.40') },
    ...(mfa ? [{ name: 'code', label: t('backup.26'), max: 6, min: 6, placeholder: t('backup.27') }] : []),
    ...(announce ? [{ node: announce.node }] : []),
  ], (announce ? announce.wrap : run => run)(async input => {
    const action = ctx.pageAction('backup-apply');
    const result = await ctx.api('/backup/restore/apply', 'POST', { uploadId: ui.uploadId, confirm: true, restoreAccess: ui.restoreAccess === true, ...(flow.setup ? flow.proof() : { password: input.password, ...(mfa ? { code: input.code } : {}) }) });
    input.password = '';
    action.check();
    ui.operation = { operationId: result.operationId, state: result.state ?? 'waiting_idle', lost: false };
    watch(flow);
  }), { save: t('backup.41'), danger: true, reload: false, safeFocus: flow.setup,
    description: t('backup.42', { people, admins }) });
}

// After a restore was accepted: follow it until this console is signed out
// (it worked: the accounts are the backup's now) or the server says it did not.
function watch(flow) {
  const { ctx } = flow, ui = uiOf(ctx), action = ctx.pageAction('backup-watch');
  ctx.announceAtLogin?.(t('backup.43'));
  flow.render();
  const tick = async () => {
    if (!action.current() || !ui.operation || ui.operation.done) return;
    try {
      const status = await ctx.api('/backup');
      if (!action.current()) return;
      const restore = status.restore ?? {};
      ui.operation.lost = false;
      if (restore.operationId !== ui.operation.operationId) {
        // The server answers but does not name this restore (its update service does not answer it): after
        // twenty such answers in a row (half a minute) the page stops waiting instead of showing progress for ever.
        if ((ui.operation.unnamed = (ui.operation.unnamed ?? 0) + 1) >= 20) {
          ui.operation.done = true; ctx.announceAtLogin?.('');
          ui.uploadId = null; ui.uploaded = null; ui.preview = null;
          flow.fail(t('backup.resultUnknownF2'));
        }
      } else {
        ui.operation.unnamed = 0;
        ui.operation.state = restore.state; ui.operation.blockers = restore.blockers ?? [];
        if (!activeStates.has(restore.state) && restore.state !== 'succeeded') {
          // It did not happen: this sign-in is still good and nothing changed.
          ui.operation.done = true; ui.operation.errorCode = restore.errorCode ?? null;
          ctx.announceAtLogin?.('');
          ui.uploadId = null; ui.uploaded = null; ui.preview = null;
          flow.fail(failureText(restore.errorCode) + (restore.state === 'rolled_back' ? t('backup.44') : restore.state === 'failed' ? t('backup.45') : ''));
        }
      }
    } catch (error) {
      if (!action.current()) return;
      // No answer while the server restarts; a 401 has already opened the sign-in page.
      ui.operation.lost = true;
    }
    if (action.current() && ui.operation && !ui.operation.done) { flow.render(); const timer = setTimeout(tick, 1500); timer.unref?.(); }
    else if (action.current()) flow.render();
  };
  const timer = setTimeout(tick, 1000); timer.unref?.();
}

function progressView(ui) {
  const operation = ui.operation, waiting = (operation.blockers ?? []).map(row => blockerText[row.code] ?? null).filter(Boolean);
  return el('div', { class: 'backup-progress', role: 'status' }, el('span', { class: 'loading-dot', 'aria-hidden': 'true' }),
    el('div', {}, el('strong', {}, t('backup.46')), el('span', {}, operation.lost ? t('backup.47') : stateText[operation.state] ?? t('backup.48')),
      waiting.length && operation.state === 'waiting_idle' ? el('small', {}, t('backup.49') + [...new Set(waiting)].join(', ')) : null));
}

function previewView(flow, status) {
  const { ctx } = flow, ui = uiOf(ctx), preview = ui.preview, counts = preview.counts;
  const elsewhere = preview.origin && preview.origin !== globalThis.location.origin;
  const rows = [
    [t('backup.50'), date(preview.createdAt)],
    [t('backup.51'), preview.origin ?? t('backup.52'), t('common.34') + preview.serverVersion],
    [t('backup.53'), t('backup.54', { accounts: counts.accounts }), counts.adminNames.length ? t('backup.55') + counts.adminNames.join(', ') + (counts.admins > counts.adminNames.length ? t('backup.56') : '') : null],
    [t('common.5'), t('backup.57', { desktops: counts.desktops })], [t('backup.58'), t('backup.59', { hosts: counts.hosts })], [t('backup.assignmentsRowF2'), t('backup.60', { assignments: counts.assignments })],
    [t('common.10'), preview.proxmox.configured ? preview.proxmox.endpoint ?? t('backup.61') : t('common.36')],
  ];
  const notes = [];
  if (!preview.proxmox.readable && preview.proxmox.configured || preview.unreadableSecrets > 0) notes.push(['is-warn', t('backup.62'), t('backup.63')]);
  notes.push(elsewhere ? ['is-warn', t('backup.64'), t('backup.65', { origin: preview.origin })]
    : ['', '', t('backup.66')]);
  const option = preview.access.available && preview.access.differs
    ? el('label', { class: 'backup-option' }, el('input', { type: 'checkbox', 'data-focus': 'backup-restore-access', checked: ui.restoreAccess === true, onchange: event => { ui.restoreAccess = event.target.checked === true; } }),
        el('span', {}, el('strong', {}, t('backup.67')), el('small', {}, t('backup.68', { primaryOrigin: preview.access.primaryOrigin }))))
    : el('p', { class: 'note' }, icon('info'), el('span', {}, t('backup.69')));
  const unsupported = status && status.restore.supported !== true;
  return el('div', { class: 'backup-preview' },
    el('h3', {}, t('backup.70')), facts(rows),
    preview.migrated ? el('p', { class: 'note' }, icon('info'), el('span', {}, t('backup.71'))) : null,
    notes.map(([tone, title, text]) => tone ? el('div', { class: 'inline-alert ' + tone, role: 'status' }, icon('alert'), el('div', {}, el('strong', {}, title), el('span', { class: 'backup-lines' }, text)))
      : el('p', { class: 'note' }, icon('info'), el('span', { class: 'backup-lines' }, text))),
    option,
    el('div', { class: 'backup-actions' },
      disable(button(t('backup.72'), () => confirmRestore(flow, status), 'danger', 'upgrade', { 'data-focus': 'backup-restore-apply' }), unsupported ? (dockerRestoreCommand(status) ? t('backup.dockerApplyHint') : messages.restore_unavailable) : null),
      button(t('backup.73'), () => forget(flow), 'quiet')));
}

function chooseView(flow, status) {
  const { ctx } = flow, ui = uiOf(ctx), busy = !!ui.busy;
  const submit = async event => {
    event.preventDefault();
    const form = event.target, file = ui.file ?? form.querySelector('input[type=file]')?.files?.[0], passphrase = form.querySelector('input[name=passphrase]').value;
    if (!file) { ui.error = t('backup.74'); flow.render(); return; }
    if (file.size > (status?.restore?.maxFileBytes ?? Infinity)) { ui.error = messages.backup_too_large; flow.render(); return; }
    await uploadAndInspect(flow, file, passphrase);
  };
  const percent = Math.round((ui.progress ?? 0) * 100);
  return el('form', { class: 'backup-choose', onsubmit: submit },
    // The browser draws a file input's own button in the browser's language ("파일 선택" in an
    // English console), so the input is hidden and the console draws the button and the name.
    el('div', { class: 'backup-file-field' }, el('span', { class: 'backup-file-title' }, t('backup.75')),
      el('label', { class: 'backup-file-pick' + (busy ? ' is-disabled' : '') },
        el('input', { type: 'file', name: 'file', class: 'visually-hidden', accept: '.mtbk', required: !ui.file, disabled: busy, 'data-focus': 'backup-file', 'aria-label': t('backup.75'),
          onchange: event => { ui.file = event.target.files?.[0] ?? null; ui.error = ''; const name = event.target.closest('.backup-file-field')?.querySelector('.backup-file-name'); if (name) name.textContent = ui.file ? `${ui.file.name} · ${bytes(ui.file.size)}` : t('backup.noFile'); } }),
        el('span', { class: 'button secondary small', 'aria-hidden': 'true' }, icon('search'), t('backup.chooseFile')),
        el('span', { class: 'backup-file-name' }, ui.file ? `${ui.file.name} · ${bytes(ui.file.size)}` : t('backup.noFile'))),
      el('small', { class: 'field-hint' }, t('backup.76'))),
    el('label', {}, t('backup.21'), el('input', { type: 'password', name: 'passphrase', required: true, minlength: 12, maxlength: 256, autocomplete: 'off', disabled: busy, 'data-focus': 'backup-passphrase' })),
    flow.extraFields?.(busy) ?? null,
    ui.error ? el('p', { class: 'error', role: 'alert' }, ui.error) : null,
    busy ? el('div', { class: 'backup-progress', role: 'status' }, el('span', { class: 'loading-dot', 'aria-hidden': 'true' }),
      el('div', {}, el('strong', {}, ui.busy === 'upload' ? t('backup.77', { percent }) : t('backup.78')), el('span', {}, ui.busy === 'upload' ? t('backup.79') : t('backup.80')))) : null,
    el('div', { class: 'backup-actions' }, disable(el('button', { type: 'submit', class: 'primary' }, icon('search'), t('backup.81')), busy ? t('backup.82') : null)));
}

// The steps of a restore, wherever they are shown.
export function restoreSteps(flow, status) {
  const ui = uiOf(flow.ctx);
  if (ui.operation && !ui.operation.done) return progressView(ui);
  return ui.preview ? previewView(flow, status) : chooseView(flow, status);
}

function lastRestore(ctx, last) {
  const proxmox = last.proxmox ?? {}, access = last.access ?? {};
  const proxmoxText = { ok: t('backup.83'), checking: t('backup.84'), none: t('backup.85'), error: t('backup.86') }[proxmox.state] ?? t('common.17');
  const accessText = { kept: t('backup.87'), pending: t('backup.88'), same: t('backup.89'), requested: t('backup.90'), failed: t('backup.91') }[access.state] ?? t('common.17');
  return panel({ title: t('backup.92'), className: 'backup-last' },
    proxmox.state === 'error' ? el('div', { class: 'inline-alert is-error', role: 'alert' }, icon('alert'), el('div', {}, el('strong', {}, t('backup.93')),
      el('span', { class: 'backup-lines' }, (messages[proxmox.errorCode] ?? t('common.37')) + t('backup.94'))), ctx.link(t('common.10'), 'proxmox', null, { class: 'button quiet' })) : null,
    facts([
      [t('backup.95'), date(last.restoredAt), last.actor === 'multitap restore' ? t('backup.dockerActor') : last.actor ? t('backup.96', { actor: last.actor }) : null],
      [t('backup.97'), date(last.backup?.createdAt), [last.backup?.origin, last.backup?.serverVersion ? t('common.34') + last.backup.serverVersion : null].filter(Boolean).join(' · ') || null],
      [t('common.10'), proxmoxText],
      [t('common.12'), accessText, access.state === 'requested' ? ctx.link(t('backup.98'), 'server-access') : access.state === 'failed' ? messages[access.errorCode] ?? null : null],
    ]));
}

export function render(ctx) {
  const entry = ctx.resource('backup'), status = entry?.value ?? null, ui = uiOf(ctx);
  const flow = { ctx, setup: false, render: ctx.render, proof: () => ({}), fail: text => ctx.notice(text, true) };
  const header = ctx.pageHeader({ title: t('common.13'), description: t('backup.99') });
  if (!status && entry?.error) return [header, el('div', { class: 'inline-alert is-error', role: 'alert' }, icon('alert'), el('div', {}, el('strong', {}, t('backup.100')), el('span', {}, entry.error)), button(t('common.32'), () => ctx.load('backup', { force: true }), 'quiet', 'refresh'))];
  // A page that was reloaded picks up where this sign-in left off: the file it
  // had checked, or the restore it had started.
  const upload = status?.upload;
  if (upload && !ui.busy && upload.uploadId !== ui.dismissed) {
    if (!ui.operation && upload.state === 'applying' && activeStates.has(status.restore.state) && status.restore.operationId === upload.operationId) {
      ui.operation = { operationId: upload.operationId, state: status.restore.state, blockers: status.restore.blockers ?? [], lost: false };
      const timer = setTimeout(() => { if (ctx.state.ui.backup === ui && ui.operation && !ui.operation.done) watch(flow); }, 0); timer.unref?.();
    } else if (!ui.preview && !ui.operation && upload.state === 'inspected' && upload.preview) { ui.preview = upload.preview; ui.uploadId = upload.uploadId; }
  }
  const restoring = ui.operation && !ui.operation.done;
  const unsupported = status && status.restore.supported !== true;
  const restore = panel({ title: t('backup.101'), className: 'backup-restore' },
    restoring ? null : el('div', { class: 'inline-alert is-warn', role: 'note' }, icon('alert'), el('div', {}, el('strong', {}, t('backup.102')),
      el('span', {}, t('backup.103')))),
    // Not beside the progress of a restore that is already running.
    unsupported && !restoring ? unsupportedNote(status) : null,
    restoreSteps(flow, status));
  return [header, restoring ? null : exportPanel(ctx, status), restore, !restoring && status?.last ? lastRestore(ctx, status.last) : null];
}

// The first setup screen: the same steps inside its card, with the one-time
// setup code from the server log in place of a password.
export function renderSetupRestore(ctx, container, { back }) {
  const ui = uiOf(ctx);
  const flow = { ctx, setup: true, proof: () => ({ setupCode: ui.setupCode ?? '' }), fail: text => { ui.error = text; },
    render: () => renderSetupRestore(ctx, container, { back }),
    extraFields: busy => el('label', {}, t('backup.105'), el('input', { name: 'setupCode', value: ui.setupCode ?? '', required: true, minlength: 12, maxlength: 20, autocomplete: 'one-time-code', spellcheck: 'false', placeholder: 'XXXX-XXXX-XXXX', disabled: busy, 'data-focus': 'backup-setup-code', oninput: event => { ui.setupCode = event.target.value; } }),
      el('small', { class: 'field-hint' }, t('backup.106'))) };
  const restoring = ui.operation && !ui.operation.done;
  // Only nodes: the browser's own replaceChildren writes a null as the text "null".
  container.replaceChildren(...[
    el('h2', {}, t('backup.107')),
    el('p', { class: 'auth-lead' }, t('backup.108')),
    // A fresh Docker server restores with its command; the file can still be checked here.
    dockerRestoreCommand(ui.status) && !restoring ? unsupportedNote(ui.status) : null,
    restoreSteps(flow, ui.status ?? null),
    restoring ? null : el('button', { type: 'button', class: 'quiet', onclick: back }, t('backup.109'))].filter(Boolean));
}
export async function loadSetupStatus(ctx, redraw = null) {
  const ui = uiOf(ctx);
  try { const status = await ctx.api('/backup'); if (validResponse(status)) ui.status = status; } catch { /* The steps say what the server answers. */ }
  if (redraw && dockerRestoreCommand(ui.status) && !ui.file && !ui.busy && !ui.preview && !ui.operation) redraw();
}
