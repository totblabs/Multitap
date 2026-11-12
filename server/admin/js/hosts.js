// Multitap Host: connection state, remote program updates (host-update-v1),
// enrollment approval and the automatic diagnostics of each VM's Host.
import { el, icon, button, stateDot, table, panel, facts, emptyState, loadingState, errorState, date, relative, until, timeTag, disable } from './dom.js';
import { messages, hostUpdateActive, hostUpdateLabels, hostUpdateCodeText, hostUpdateDescription, hostUpdateStage, displayReason, activationReason, recoveryLabels, nativePhases, consoleReasons } from './labels.js';
import { hostEntries } from './timeline.js';
import { announceFirst, announcements } from './notices.js';
import { t } from './i18n.js';

const vmOrder = (a, b) => Number(a.pve_vmid) - Number(b.pve_vmid);
export const localPc = desktop => desktop?.kind === 'pc' && desktop.localPcApproved === true;
export const hostUpdateView = (ctx, desktop) => ctx.state.data?.hostUpdates?.desktops?.[desktop.id] ?? null;
const hostUpdateBusy = view => hostUpdateActive.has(view?.operation?.status);
export const enrolledHost = (ctx, desktop) => {
  const rows = ctx.value('hosts')?.hosts;
  return Array.isArray(rows) ? rows.find(host => host?.desktopId === desktop.id) ?? null : null;
};
// Whether a desktop's Host answers now, and the version it runs: the Host list the console keeps reading
// (GET /hosts) where it has the desktop, else the inventory as it was last read. The inventory alone keeps what
// was true when the page opened: a Host that connected after its approval would stay 응답 없음.
export const hostConnected = (ctx, desktop) => { const row = enrolledHost(ctx, desktop); return (typeof row?.connected === 'boolean' ? row.connected : desktop?.host?.connected) === true; };
export const hostVersion = (ctx, desktop) => { const row = enrolledHost(ctx, desktop); return (row?.connected === true ? row.version : null) ?? desktop?.host?.version ?? row?.version ?? null; };
// Registration, not a runtime-connected flag, determines list membership.
export function registeredHostIds(ctx) {
  const rows = ctx.value('hosts')?.hosts;
  if (ctx.resource('hosts')?.error || !Array.isArray(rows) || rows.some(row => typeof row?.desktopId !== 'string' || !row.desktopId)) return null;
  const ids = new Set(rows.map(row => row.desktopId));
  return ids.size === rows.length ? ids : null;
}
// A Host registration id as the server issues it (DELETE /desktops/:id/host takes it as expectedHostId; desktops.js deleteDesktops).
export const validHostId = id => typeof id === 'string' && /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i.test(id);
const windowsCapable = desktop => !['linux', 'other'].includes(desktop.pve_os_family);

// One word for a registered desktop, read the same way by the list, the home
// page and 지금 할 일: bad (nobody can connect and the operator can fix it),
// live (someone is connected), ok, off. A local PC that does not answer is
// most likely switched off, and Proxmox does not report its power.
export function desktopStatus(ctx, desktop) {
  const state = hostState(ctx, desktop), host = state.host, connected = hostConnected(ctx, desktop);
  const display = host?.connected ? host.diagnostics?.display : null;
  const live = (ctx.value('sessions')?.sessions ?? []).filter(session => session.desktopId === desktop.id);
  const base = { state, live, why: null, cause: null };
  if (display?.rebootRequired || display?.recoveryRequired) return { ...base, key: 'bad', label: t('common.94'), tone: 'red', cause: display.rebootRequired ? 'reboot' : 'recovery', why: display.rebootRequired ? t('common.54') : t('common.55') };
  if (connected) return live.length ? { ...base, key: 'live', label: t('common.49'), tone: 'green' } : { ...base, key: 'ok', label: t('common.95'), tone: 'blue' };
  if (desktop.kind === 'pc') return { ...base, key: 'off', label: t('common.23'), tone: '', cause: 'pc-silent', why: t('hosts.8') };
  if (desktop.pve_power_status === 'stopped') return { ...base, key: 'off', label: t('common.96'), tone: '' };
  return { ...base, key: 'bad', label: t('common.94'), tone: 'red', cause: 'silent', why: t('common.23') + (host?.lastSeenAt ? t('hosts.9', { relative: relative(host.lastSeenAt) }) : '') };
}
export const statusOrder = { bad: 0, live: 1, ok: 2, off: 3 };

// online · outdated (a newer Host is published) · offline (enrolled, not
// connected) · none (never enrolled) · unknown (enrollment list not loaded).
export function hostState(ctx, desktop) {
  const view = hostUpdateView(ctx, desktop), host = enrolledHost(ctx, desktop), loaded = !!ctx.value('hosts'), connected = hostConnected(ctx, desktop);
  if (connected) return view?.newer ? { kind: 'outdated', label: t('common.90'), tone: 'amber', host } : { kind: 'online', label: t('common.95'), tone: 'green', host };
  if (host) return desktop.kind !== 'pc' && desktop.pve_power_status === 'stopped' ? { kind: 'offline', label: t('common.96'), tone: '', host } : { kind: 'offline', label: t('common.23'), tone: 'red', host };
  return loaded ? { kind: 'none', label: t('hosts.11'), tone: '', host: null } : { kind: 'unknown', label: t('hosts.12'), tone: '', host: null };
}
// A remote update that ended without installing (the desktop window says so above its form table, with 다시 시도).
export const hostUpdateFailed = view => !!view?.operation && view.operation.status !== 'completed' && !hostUpdateActive.has(view.operation.status) && !hostUpdateSettled(view);
// An attempt that ended without installing is over once the Host runs its target version or a newer one (the
// installer run on the desktop itself, as the failure's own text asks): nothing is left to report or to retry.
const versionParts = value => typeof value === 'string' && /^\d{1,6}\.\d{1,6}\.\d{1,6}$/.test(value) ? value.split('.').map(Number) : null;
function hostUpdateSettled(view) {
  const operation = view?.operation, running = view?.connected === true ? versionParts(view.version) : null, target = versionParts(operation?.targetVersion);
  if (!operation || operation.status === 'completed' || hostUpdateActive.has(operation.status) || !running || !target) return false;
  for (let i = 0; i < 3; i++) if (running[i] !== target[i]) return running[i] > target[i];
  return true;
}
// What the Host's last update says, in its own words: the reason of a failure, a restart it waits for.
export const hostUpdateDetail = operation => [hostUpdateCodeText[operation?.code], operation?.rebootRequired ? hostUpdateCodeText.restart_required : null].filter(Boolean).join('\n');
// The update running now, or how the last one ended, as a few plain words (with the download's progress).
function hostUpdateNote(view) {
  const operation = view?.operation;
  if (!operation || hostUpdateSettled(view)) return null;
  const label = hostUpdateLabels[operation.status] ?? operation.status;
  const text = operation.status === 'downloading' && Number.isInteger(operation.progress) ? `${label} ${operation.progress}%` : label;
  const tone = operation.status === 'completed' ? (operation.rebootRequired || operation.code === 'display_attention' ? 'is-warn' : '') : hostUpdateActive.has(operation.status) ? '' : 'is-bad';
  const progress = operation.status === 'downloading' && Number.isInteger(operation.progress) ? el('progress', { class: 'host-update-progress', max: '100', value: String(operation.progress), 'aria-label': t('hosts.13', { progress: operation.progress }) }) : null;
  return [el('span', { class: 'host-note' + (tone ? ' ' + tone : ''), title: [operation.targetVersion ? 'Host ' + operation.targetVersion : null, hostUpdateDetail(operation)].filter(Boolean).join('\n') || null }, text), progress];
}
// The Host in a form table: its version, then in plain words what waits for it, each after a dot (pages.css
// .host-note): an update (amber), a remote update it cannot take, or the update running now.
export function hostCell(ctx, desktop) {
  const view = hostUpdateView(ctx, desktop), state = hostState(ctx, desktop), connected = hostConnected(ctx, desktop);
  const version = connected ? view?.version ?? hostVersion(ctx, desktop) : state.host?.version;
  const note = (text, cls, title) => el('span', { class: 'host-note ' + cls, title }, text);
  const running = hostUpdateNote(view);
  const offer = !running && view?.available && !hostUpdateBusy(view) ? note(t('hosts.updateWaiting'), 'is-warn', t('hosts.14', { latestVersion: view.latestVersion })) : null;
  const unsupported = connected && view && !view.supported && view.newer ? note(t('hosts.16'), 'muted', messages.host_update_unsupported) : null;
  return el('span', { class: 'host-cell' }, el('span', { class: 'host-version' }, version ?? '—'), offer, running, unsupported);
}
export function hostUpdateButton(ctx, desktop, cls = 'quiet') {
  const view = hostUpdateView(ctx, desktop);
  if (!view?.connected || !view.newer || hostUpdateBusy(view)) return null;
  // After a run that did not finish, the same request is offered as a retry.
  const retry = hostUpdateFailed(view);
  const node = button(retry ? t('common.41') : t('common.33'), () => hostUpdateForm(ctx, desktop), cls, retry ? 'refresh' : 'download', retry ? { 'aria-label': t('hosts.22') } : {});
  if (!view.supported) { node.disabled = true; node.title = messages.host_update_unsupported; }
  return node;
}
export const hostUpdateEligible = ctx => {
  const registered = registeredHostIds(ctx);
  return (ctx.state.data?.desktops ?? []).filter(desktop => { const view = hostUpdateView(ctx, desktop); return registered?.has(desktop.id) && view?.available; });
};
// The button that updates every waiting Host: with one desktop it does not say "all".
export const hostUpdateAllLabel = count => count === 1 ? t('hosts.updateOneF1') : t('common.100', { eligibleCount: count });
function hostUpdateNotes(latest) { return el('div', { class: 'host-update-notes-field' }, el('span', {}, t('common.104')), el('pre', { class: 'host-update-notes' }, latest.changelog?.trim() || t('hosts.23'))); }
export function hostUpdateForm(ctx, desktop) {
  const view = hostUpdateView(ctx, desktop), latest = ctx.state.data?.hostUpdates?.latest;
  if (!view?.available || !latest || latest.version !== view.latestVersion) return;
  const action = ctx.pageAction('host-update:' + desktop.id);
  let revision;
  const submit = async disconnectSessions => {
    action.check();
    const result = await ctx.api('/desktops/' + encodeURIComponent(desktop.id) + '/host-update', 'POST', { version: latest.version, disconnectSessions });
    action.check();
    if (ctx.state.data?.hostUpdates?.desktops) ctx.state.data.hostUpdates.desktops[desktop.id] = result.hostUpdate;
    // 지금 업데이트(연결 끊김) is also 'waiting' for a moment, while the Host ends the session: it does not wait for the connection.
    if (ctx.modalCurrent(revision)) ctx.notice(!disconnectSessions && result.hostUpdate.operation?.status === 'waiting' ? t('hosts.24', { name: desktop.name, version: latest.version }) : t('hosts.25', { name: desktop.name, version: latest.version }));
    ctx.scheduleHostUpdatePoll();
  };
  const fields = [{ label: localPc(desktop) ? t('hosts.26') : t('hosts.27'), value: `${localPc(desktop) ? t('common.22') : `VM ${desktop.pve_vmid}`} · ${desktop.name}`, readonly: true }, { label: t('hosts.28'), value: `${view.version} → ${latest.version}`, readonly: true }, { node: hostUpdateNotes(latest) }];
  if (!view.sessionActive) { revision = ctx.modal(t('common.33'), fields, () => submit(false), { save: t('common.8'), description: hostUpdateDescription, reload: false }); return; }
  // An active session: wait for it to end, or a second explicit choice ends it now.
  // 먼저 알리기 (js/notices.js) applies to that second choice only: waiting for the connection ends nothing.
  const people = new Set((ctx.value('sessions')?.sessions ?? []).filter(session => session.desktopId === desktop.id && session.account).map(session => session.account)).size;
  const announce = announceFirst(ctx, { audience: { target: { kind: 'desktop', desktopIds: [desktop.id] }, people: people || null }, text: announcements.hostUpdate });
  const now = button(t('hosts.29'), () => ctx.submitModal(revision, announce ? announce.wrap(() => submit(true)) : () => submit(true), [now]), 'danger');
  if (announce) fields.push({ node: announce.node });
  fields.push({ node: el('div', { class: 'host-update-now' }, el('p', { class: 'field-hint' }, t('hosts.30')), now) });
  revision = ctx.modal(t('common.33'), fields, () => submit(false), { save: t('hosts.31'), description: hostUpdateDescription, reload: false, submitControls: [now] });
}
export function hostUpdateAll(ctx) {
  const latest = ctx.state.data?.hostUpdates?.latest, eligible = hostUpdateEligible(ctx);
  if (!latest || !eligible.length) return;
  const busyDesktops = eligible.filter(desktop => hostUpdateView(ctx, desktop).sessionActive), busy = busyDesktops.length;
  const idleDesktops = eligible.filter(desktop => !busyDesktops.includes(desktop));
  const who = (ctx.value('sessions')?.sessions ?? []).filter(session => session.account && busyDesktops.some(desktop => desktop.id === session.desktopId))
    .map(session => `${session.account} (${busyDesktops.find(desktop => desktop.id === session.desktopId).name})`);
  // The result panel (데스크톱 and 설정) follows each started Host until it has its result.
  const started = (ids, skipped, refused = []) => {
    ctx.state.ui.hostBatch = { version: latest.version, ids, skipped };
    ctx.notice(t('hosts.34', { version: latest.version, idsCount: ids.length, value: skipped ? t('hosts.32', { skipped }) : '', value2: refused.length ? t('hosts.33', { refusedCount: refused.length, join: refused.join(', ') }) : '' }), refused.length > 0);
    ctx.scheduleHostUpdatePoll();
  };
  let revision;
  // 먼저 알리기 (js/notices.js): the people on the desktops whose connections this ends.
  const busyPeople = new Set((ctx.value('sessions')?.sessions ?? []).filter(session => session.account && busyDesktops.some(desktop => desktop.id === session.desktopId)).map(session => session.account)).size;
  const announce = announceFirst(ctx, { audience: busy ? { target: { kind: 'desktop', desktopIds: busyDesktops.map(desktop => desktop.id) }, people: busyPeople || null } : null, text: announcements.hostUpdate });
  // ‘접속 안 한 N대만’: the same single-Host request as each desktop's own
  // update button, for the desktops nobody is connected to. No connection ends.
  const idleOnly = idleDesktops.length && busy ? button(t('hosts.35', { idleDesktopsCount: idleDesktops.length }), () => ctx.submitModal(revision, async () => {
    const ids = [], refused = [];
    let reason = '';
    for (const desktop of idleDesktops) {
      try {
        const result = await ctx.api('/desktops/' + encodeURIComponent(desktop.id) + '/host-update', 'POST', { version: latest.version, disconnectSessions: false });
        if (ctx.state.data?.hostUpdates?.desktops) ctx.state.data.hostUpdates.desktops[desktop.id] = result.hostUpdate;
        ids.push(desktop.id);
      } catch (error) { if (error.code === 'admin_session_changed') throw error; refused.push(desktop.name); reason ||= error.message; }
    }
    if (!ids.length) throw new Error(t('hosts.36', { reason }));
    started(ids, 0, refused);
  }, [idleOnly]), 'secondary') : null;
  revision = ctx.modal(t('hosts.37', { eligibleCount: eligible.length }), [
    { node: el('p', { class: 'confirm-who' + (busy ? '' : ' is-calm') }, icon('user'), el('span', {}, busy ? t('hosts.38', { value: who.length || busy, value2: who.length ? t('common.105') : t('common.93') }) : ctx.value('sessions') ? t('common.43') : t('common.44'))) },
    ...(who.length ? [{ node: el('p', { class: 'modal-description' }, who.join(' · ')) }] : []),
    { node: el('p', { class: 'modal-description' }, t('hosts.39', { version: latest.version })) },
    { node: el('details', { class: 'disclosure' }, el('summary', {}, t('hosts.40')), hostUpdateNotes(latest)) },
    ...(announce ? [{ node: announce.node }] : []),
    ...(idleOnly ? [{ node: el('div', { class: 'modal-extra modal-choice' }, el('span', {}, el('strong', {}, t('hosts.41')), el('small', {}, t('hosts.42', { busy }))), idleOnly) }] : []),
  ], (announce ? announce.wrap : run => run)(async () => {
    const result = await ctx.api('/host-updates/all', 'POST', { version: latest.version });
    const rows = result.results.filter(row => row.started);
    // The server answers a row for every registered desktop: only the ones this dialog offered count as left out.
    started(rows.map(row => row.desktopId), eligible.filter(desktop => !rows.some(row => row.desktopId === desktop.id)).length);
  }), { save: busy ? t('hosts.43', { eligibleCount: eligible.length }) : t('hosts.44', { eligibleCount: eligible.length }), submitControls: idleOnly ? [idleOnly] : [] });
}
// The last bulk update: a row per Host it started with its stage and short
// reason, and the counts, then the result once every Host has one.
export function hostBatchPanel(ctx, batch, onClose) {
  const desktops = batch.ids.map(id => ctx.state.data.desktops.find(d => d.id === id)).filter(Boolean);
  const stageOf = d => hostUpdateStage(hostUpdateView(ctx, d)) ?? { label: t('common.17'), tone: 'amber' };
  const rows = desktops.map(d => {
    const operation = hostUpdateView(ctx, d)?.operation, stage = stageOf(d);
    const percent = operation?.status === 'downloading' && Number.isInteger(operation.progress) ? operation.progress : null;
    const reason = stage.label === t('common.106') ? t('hosts.45') : [hostUpdateCodeText[operation?.code], operation?.rebootRequired ? hostUpdateCodeText.restart_required : null].filter(Boolean).join('\n');
    return [el('span', { class: 'vm-number' }, localPc(d) ? t('common.22') : `VM ${d.pve_vmid}`), el('strong', {}, d.name),
      el('div', { class: 'host-update-status' }, stateDot(percent === null ? stage.label : `${stage.label} ${percent}%`, stage.tone, { 'data-stage': stage.label }), percent === null ? null : el('progress', { class: 'host-update-progress', max: '100', value: String(percent), 'aria-label': t('hosts.46', { name: d.name, percent }) })),
      reason ? el('small', { class: 'muted host-batch-reason' }, reason) : '—'];
  });
  const count = label => desktops.filter(d => stageOf(d).label === label).length;
  const running = [t('common.107'), t('common.9'), t('common.108'), t('common.106')].reduce((sum, label) => sum + count(label), 0);
  const summary = running ? t('hosts.47', { running, count: count(t('common.109')), count2: count(t('common.110')) }) + (count(t('common.17')) ? ' · ' + t('hosts.51', { count: count(t('common.17')) }) : '')
    : [t('hosts.48'), t('hosts.49', { count: count(t('common.109')) }), t('hosts.50', { count: count(t('common.110')) }), count(t('common.17')) ? t('hosts.51', { count: count(t('common.17')) }) : null].filter(Boolean).join(' · ');
  return panel({ title: t('hosts.52', { version: batch.version }), count: desktops.length, actions: [button(t('common.31'), onClose, 'quiet')], className: 'host-batch' },
    el('p', { class: 'host-batch-summary', role: 'status' }, summary + (batch.skipped ? t('hosts.53', { skipped: batch.skipped }) : '')),
    table([t('common.62'), t('common.5'), t('common.61'), t('hosts.54')], rows, t('hosts.55')));
}

// Enrollment requests: a Host asks to join; the administrator confirms the
// Windows name and Proxmox VM. Approval re-verifies the VM identity.
// host-request-code-v1: anyone who knows a VM's SMBIOS UUID can file a request
// for it, so each request shows a verification code that the Host's own
// settings window also shows; several live claims on one VM are flagged.
const codeShown = request => request.codeDisplay === 'shown';
const rivalsOf = request => Array.isArray(request.duplicateRequestIds) ? request.duplicateRequestIds : [];
function codeCollision(ctx, request) {
  const all = ctx.value('hostRequests')?.requests ?? [];
  return rivalsOf(request).some(id => all.find(other => other.requestId === id)?.verificationCode === request.verificationCode);
}
const requestBinding = request => ({ requestId: request.requestId, machineUuid: request.machineUuid, verificationCode: request.verificationCode, codeDisplay: request.codeDisplay, expiresAt: request.expiresAt, rivals: [...rivalsOf(request)].sort() });
// A request from a VM that never had a Host: the single VM the server found
// in Proxmox for the request's SMBIOS UUID (vmLookup, host-vm-lookup.mjs), so
// node and VM ID need no typing. Typing stays behind ‘직접 입력’ for a request
// the server could not place.
const foundVm = request => {
  const vm = request?.vmLookup?.state === 'matched' ? request.vmLookup.vm : null;
  return vm && typeof vm.node === 'string' && vm.node && Number.isSafeInteger(vm.vmid) ? vm : null;
};
const foundVmLabel = vm => `VM ${vm.vmid} · ${vm.name || `VM ${vm.vmid}`} (${vm.node})`;
// The VM a request can be approved for without typing: { vmid, name } or null.
export function requestVm(request) {
  const candidates = Array.isArray(request?.candidates) ? request.candidates : [];
  return candidates.length === 1 ? candidates[0] : candidates.length ? null : foundVm(request);
}
// Why node and VM ID have to be typed, in one line; null while the server is still looking.
function manualReason(request, candidates) {
  const lookup = request.vmLookup;
  if (candidates.length > 1 || lookup?.state === 'ambiguous') return t('hosts.56');
  if (lookup?.state === 'searching') return null;
  return lookup?.state === 'unavailable' && lookup.errorCode !== 'proxmox_lookup_incomplete' ? t('hosts.57') : t('hosts.58');
}
function approvalOwner(ctx, request) {
  const bound = requestBinding(request), signature = JSON.stringify(bound), actor = ctx.state.user, csrf = ctx.state.csrf;
  const action = ctx.pageAction('host-request:' + bound.requestId);
  return () => {
    action.check();
    if (ctx.state.user !== actor || ctx.state.csrf !== csrf) throw new Error(messages.session_expired);
    const entry = ctx.resource('hostRequests'), rows = ctx.value('hostRequests')?.requests;
    if (entry?.error || !Array.isArray(rows)) throw new Error(messages.host_request_conflict);
    const matching = rows.filter(row => row?.requestId === bound.requestId), fresh = matching[0];
    if (matching.length !== 1 || fresh.status !== 'pending' || !(Date.parse(fresh.expiresAt) > Date.now())) throw new Error(messages.host_request_conflict);
    if (JSON.stringify(requestBinding(fresh)) !== signature) throw new Error(messages.host_request_pc_target_changed);
    if (codeCollision(ctx, fresh)) throw new Error(messages.host_request_code_collision);
    return fresh;
  };
}
function approveRequest(ctx, request, matching, { manual = false } = {}) {
  const found = !manual && matching.length === 0 ? foundVm(request) : null;
  const auto = found ? Object.freeze({ node: found.node, vmid: found.vmid, name: found.name, hasExistingHost: !!found.hasExistingHost }) : null;
  const explicit = !auto && matching.length !== 1, known = !explicit && !auto ? Object.freeze({ ...matching[0] }) : null;
  // Every approval compares the code with the Host screen (server: host_request_code_required).
  if (!codeShown(request) || !request.verificationCode?.trim()) return;
  const current = approvalOwner(ctx, request), requestId = request.requestId;
  const responseOwner = ctx.pageAction('host-approval-response:' + requestId);
  try { current(); } catch (error) { ctx.notice(error.message, true); return; }
  const contested = rivalsOf(request).length > 0;
  let submitting = false, committed = false;
  ctx.modal(t('hosts.59'), [
    { label: t('common.66'), value: request.hostname, readonly: true },
    { label: t('hosts.60'), value: el('strong', { class: 'mono-value request-code-value' }, request.verificationCode), readonly: true, hint: t('hosts.61') },
    { name: 'acknowledgeCode', label: t('hosts.codeMatch'), checkbox: true },
    { label: t('hosts.63'), value: el('code', { class: 'mono-value' }, request.machineUuid), readonly: true },
    ...(explicit ? [
      { name: 'node', label: t('hosts.64'), value: '', max: 128, hint: t('hosts.65') },
      { name: 'vmid', label: 'VM ID', value: '', max: 9, hint: t('hosts.66') },
    ] : auto ? [{ label: t('hosts.70'), value: foundVmLabel(auto), readonly: true, hint: t('hosts.71') }]
      : [{ name: 'desktopId', label: t('hosts.70'), value: known.desktopId, options: [[known.desktopId, `VM ${known.vmid} · ${known.name}`]] }]),
    ...(explicit || known?.hasExistingHost || auto?.hasExistingHost ? [{ name: 'replaceExisting', label: t('hosts.72'), value: 'no', options: [['no', t('hosts.73')], ['yes', t('hosts.74')]], hint: t('hosts.75') }] : []),
    ...(contested ? [{ name: 'acknowledgeDuplicates', label: t('hosts.76'), value: 'no', options: [['no', t('hosts.68')], ['yes', t('hosts.77')]], hint: t('hosts.78', { rivalsOf: rivalsOf(request).length }) }] : []),
  ], async input => {
    if (submitting || committed) return;
    const fresh = current();
    if (input.acknowledgeCode !== 'yes') throw new Error(messages.host_request_code_required);
    let target;
    if (explicit) {
      const node = String(input.node ?? '').trim(), text = String(input.vmid ?? '').trim(), vmid = Number(text);
      if (!/^[A-Za-z0-9_.-]{1,128}$/.test(node) || node === '.' || node === '..' || !/^[0-9]{3,9}$/.test(text) || !Number.isSafeInteger(vmid) || vmid < 100 || vmid > 999999999) throw new Error(messages.host_vm_mapping_required);
      target = { node, vmid };
    } else if (auto) {
      // The server found this VM; it checks at approval that it still finds the same one.
      const now = (fresh.candidates ?? []).length ? null : foundVm(fresh);
      if (!now || now.node !== auto.node || now.vmid !== auto.vmid || !!now.hasExistingHost !== auto.hasExistingHost) throw new Error(messages.host_request_match_changed);
      target = { node: auto.node, vmid: auto.vmid, serverMatch: true };
    } else {
      if (input.desktopId !== known.desktopId || !(fresh.candidates ?? []).some(row => row.desktopId === known.desktopId && row.vmid === known.vmid && row.node === known.node && !!row.hasExistingHost === !!known.hasExistingHost)) throw new Error(messages.desktop_changed);
      target = { desktopId: known.desktopId };
    }
    if (contested && input.acknowledgeDuplicates !== 'yes') throw new Error(messages.host_request_duplicate);
    const body = { ...target, acknowledgeCode: true, replaceExisting: input.replaceExisting === 'yes', ...(contested ? { acknowledgeDuplicates: true } : {}) };
    submitting = true;
    try {
      await ctx.api('/hosts/requests/' + encodeURIComponent(requestId) + '/approve', 'POST', body);
      // A committed approval changes the request status. Only the session
      // owner is rechecked after the response; it must never be resubmitted.
      committed = true;
      responseOwner.check();
      ctx.notice(t('hosts.79'));
      void ctx.load('hostRequests', { force: true }); void ctx.load('hosts', { force: true });
    } finally { submitting = false; }
  }, { save: t('hosts.80'), description: t('hosts.81') });
}
function rejectRequest(ctx, request) {
  ctx.confirmAction(t('hosts.82'), t('hosts.83', { hostname: request.hostname }), async () => {
    await ctx.api('/hosts/requests/' + encodeURIComponent(request.requestId) + '/reject', 'POST', {}); ctx.notice(t('hosts.84'));
    void ctx.load('hostRequests', { force: true });
  }, t('hosts.85'), true);
}
function pcRequestBlocked(ctx, request) {
  if (request.status !== 'pending' || !(Date.parse(request.expiresAt) > Date.now())) return messages.host_request_expired;
  if ((request.candidates ?? []).length) return messages.host_request_vm_mismatch;
  if (!codeShown(request) || typeof request.verificationCode !== 'string' || !request.verificationCode.trim()) return messages.host_request_pc_code_required;
  if (request.localPcRegistration?.allowed !== true) return messages[request.localPcRegistration?.reason] ?? messages.host_request_pc_not_allowed;
  return null;
}
function approvePcRequest(ctx, request) {
  if (pcRequestBlocked(ctx, request)) return;
  const userId = ctx.state.user?.id, csrf = ctx.state.csrf;
  const requestId = request.requestId, verificationCode = request.verificationCode;
  const existingId = request.localPcRegistration.existingDesktopId;
  const existing = existingId ? Object.freeze({ id: existingId, name: request.localPcRegistration.existingName || ctx.state.data.desktops.find(d => localPc(d) && d.id === existingId)?.name || t('hosts.86') }) : null;
  const current = approvalOwner(ctx, request), responseOwner = ctx.pageAction('host-pc-approval-response:' + requestId);
  try { current(); } catch (error) { ctx.notice(error.message, true); return; }
  let submitting = false, committed = false;
  ctx.modal(t('hosts.87'), [
    { label: t('common.66'), value: request.hostname, readonly: true },
    { label: t('hosts.60'), value: el('strong', { class: 'mono-value request-code-value' }, request.verificationCode), readonly: true, hint: t('hosts.88') },
    { name: 'name', label: t('hosts.89'), value: existing?.name ?? request.hostname, max: 80 },
    { name: 'acknowledgeCode', label: t('hosts.67'), value: 'no', options: [['no', t('hosts.68')], ['yes', t('hosts.69')]] },
    { name: 'acknowledgeUnlinked', label: t('hosts.90'), value: 'no', options: [['no', t('hosts.68')], ['yes', t('hosts.91')]] },
    ...(existing ? [
      { name: 'desktopId', label: t('hosts.92'), value: '', options: [['', t('hosts.93')], [existing.id, existing.name]], hint: t('hosts.94') },
      { name: 'replaceExisting', label: t('hosts.72'), value: 'no', options: [['no', t('hosts.73')], ['yes', t('hosts.74')]], hint: t('hosts.95') },
    ] : []),
  ], async input => {
    if (submitting || committed) return;
    if (ctx.state.user?.id !== userId || ctx.state.csrf !== csrf) throw new Error(messages.session_expired);
    const fresh = current();
    if (!fresh || fresh.verificationCode !== verificationCode || fresh.localPcRegistration?.existingDesktopId !== existingId) throw new Error(messages.host_request_pc_target_changed);
    const blocked = pcRequestBlocked(ctx, fresh);
    if (blocked) throw new Error(blocked);
    if (input.acknowledgeCode !== 'yes') throw new Error(messages.host_request_pc_code_required);
    if (input.acknowledgeUnlinked !== 'yes') throw new Error(messages.local_pc_unlinked_confirmation_required);
    const name = String(input.name ?? '').trim();
    if (!name || name.length > 80 || /[\u0000-\u001f\u007f]/.test(name)) throw new Error(messages.host_request_pc_name_required);
    if (existing && (input.desktopId !== existing.id || input.replaceExisting !== 'yes')) throw new Error(messages.host_replacement_confirmation_required);
    submitting = true;
    try {
      await ctx.api('/hosts/requests/' + encodeURIComponent(requestId) + '/approve-pc', 'POST', { name, acknowledgeCode: true, acknowledgeUnlinked: true, ...(existing ? { desktopId: existing.id, replaceExisting: true } : {}) });
      committed = true; responseOwner.check();
      if (ctx.state.user?.id !== userId || ctx.state.csrf !== csrf) return;
      ctx.notice(t('hosts.96'));
      void ctx.load('hostRequests', { force: true }); void ctx.load('hosts', { force: true });
    } finally { submitting = false; }
  }, { save: existing ? t('hosts.97') : t('hosts.98'), description: t('hosts.99') });
}
export function renamePc(ctx, desktop) {
  if (!localPc(desktop)) return;
  ctx.modal(t('hosts.100'), [{ name: 'name', label: t('common.60'), value: desktop.name, max: 80 }], async input => {
    const name = String(input.name ?? '').trim();
    if (!name || name.length > 80 || /[\u0000-\u001f\u007f]/.test(name)) throw new Error(messages.host_request_pc_name_required);
    await ctx.api('/desktops/' + encodeURIComponent(desktop.id), 'PUT', { name });
    ctx.notice(t('hosts.101'));
  });
}
// The approve control of one request, with the reason it is unavailable. The
// request cards and the dashboard's to-do list share it, so both open the
// same confirmation dialog and neither approves on its own.
export function requestApproveButton(ctx, request, { desktop = null, kind = null, label = null, manual = false } = {}) {
  if (kind === 'pc') return disable(button(label ?? t('hosts.87'), () => approvePcRequest(ctx, request), 'primary', 'check'), pcRequestBlocked(ctx, request));
  const candidates = Array.isArray(request.candidates) ? request.candidates : [];
  const matching = desktop ? candidates.filter(item => item.desktopId === desktop.id) : candidates;
  const expired = Date.parse(request.expiresAt) <= Date.now();
  const rivals = rivalsOf(request), collision = rivals.length > 0 && codeCollision(ctx, request);
  // manual: the small ‘직접 입력’ link that opens the node and VM ID fields.
  const typed = matching.length !== 1 && (manual || desktop || !foundVm(request));
  return disable(button(label ?? (manual ? t('hosts.102') : t('common.111')), () => approveRequest(ctx, request, matching, { manual: typed }), manual ? 'quiet request-manual' : 'primary', manual ? null : 'check'),
    expired ? t('hosts.103') : !codeShown(request) || !request.verificationCode?.trim() ? messages.host_request_code_required
      : collision ? t('hosts.104') : null);
}
export const rejectHostRequest = (ctx, request) => rejectRequest(ctx, request);
export function requestCards(ctx, { desktop = null, kind = null } = {}) {
  const entry = ctx.resource('hostRequests');
  if (entry?.error || !entry?.value) return entry?.error ? errorState(t('hosts.106'), entry.error, () => ctx.load('hostRequests', { force: true })) : loadingState(t('hosts.107'));
  if (!Array.isArray(entry.value.requests)) return errorState(t('hosts.106'), messages.host_request_conflict, () => ctx.load('hostRequests', { force: true }));
  const requests = entry.value.requests.filter(request => request.status === 'pending' && (!desktop || (request.candidates ?? []).some(item => item.desktopId === desktop.id)) && (kind !== 'pc' || !(request.candidates ?? []).length && !foundVm(request) && request.localPcRegistration));
  if (!requests.length) return emptyState(t('hosts.108'), kind === 'pc' ? t('hosts.109') : desktop ? t('hosts.110') : t('hosts.111'), { iconName: 'link', compact: true });
  return el('div', { class: 'request-list', 'aria-live': 'polite' }, requests.map(request => {
    const candidates = Array.isArray(request.candidates) ? request.candidates : [];
    const matching = desktop ? candidates.filter(item => item.desktopId === desktop.id) : candidates;
    const expired = Date.parse(request.expiresAt) <= Date.now();
    const rivals = rivalsOf(request), collision = rivals.length > 0 && codeCollision(ctx, request);
    // Placed by the table, or by the server's Proxmox lookup: one approve
    // button. Otherwise the reason in one line and the ‘직접 입력’ link.
    const found = !desktop && kind !== 'pc' && matching.length === 0 ? foundVm(request) : null;
    const typed = !desktop && kind !== 'pc' && matching.length !== 1 && !found, reason = typed ? manualReason(request, matching) : null;
    const approve = typed ? null : requestApproveButton(ctx, request, { desktop, kind });
    // A request the server could not place on a VM may be a PC outside Proxmox.
    const asPc = typed && reason && request.localPcRegistration ? requestApproveButton(ctx, request, { kind: 'pc' }) : null;
    const code = request.verificationCode ? el('div', { class: 'request-code' }, el('span', {}, t('hosts.60')), el('strong', { class: 'mono-value request-code-value' }, request.verificationCode),
      el('small', { class: 'field-hint' + (codeShown(request) ? ' is-routine' : '') }, codeShown(request) ? (kind === 'pc' ? t('hosts.112') : t('hosts.113'))
        : t('hosts.114'))) : null;
    const duplicate = rivals.length ? el('p', { class: 'request-duplicate', role: 'note' }, icon('alert'), el('span', {}, t('hosts.116', { value: kind === 'pc' ? t('hosts.115') : 'VM(SMBIOS UUID)', value2: rivals.length + 1, value3: kind === 'pc' ? t('common.112') : 'VM' }))) : null;
    return el('article', { class: 'request-card' + (matching.length || found ? '' : ' is-unmatched') + (rivals.length ? ' is-contested' : '') },
      el('div', { class: 'request-main' }, el('span', { class: 'request-icon' }, icon('server')), el('div', {}, el('strong', {}, request.hostname), el('small', {}, `Host ${request.version} · `, el('time', { datetime: request.expiresAt, title: date(request.expiresAt) }, expired ? t('common.113') : t('hosts.117', { until: until(request.expiresAt) }))))),
      code, el('details', { class: 'request-more', 'data-keep-open': 'request-id:' + request.requestId }, el('summary', {}, t('hosts.118')), el('p', {}, 'SMBIOS UUID ', el('code', { class: 'request-identity', title: 'SMBIOS UUID' }, request.machineUuid))), duplicate,
      el('p', { class: 'request-match' + (matching.length === 1 || found ? '' : ' no-match') }, matching.length === 1 ? [icon('check'), el('span', {}, t('hosts.119'), matching.map(item => `VM ${item.vmid} · ${item.name}${item.hasExistingHost ? t('hosts.120') : ''}`).join(' / '))]
        : found ? [icon('check'), el('span', {}, t('hosts.121'), foundVmLabel(found) + (found.hasExistingHost ? t('hosts.120') : ''))]
        : typed ? [icon('alert'), el('span', {}, reason ?? t('hosts.122')), reason ? requestApproveButton(ctx, request, { manual: true }) : null]
        : [icon('alert'), el('span', {}, kind === 'pc' ? (pcRequestBlocked(ctx, request) ?? t('hosts.123')) : t('hosts.124'))]),
      asPc ? el('p', { class: 'field-hint request-pc-hint' }, t('hosts.125')) : null,
      el('div', { class: 'actions' }, approve, asPc, button(t('common.114'), () => rejectRequest(ctx, request), 'danger')));
  }));
}
// Installation guide with this server's address (and its own request list).
export async function enrollmentGuide(ctx, desktop = null) {
  const userId = ctx.state.user?.id, csrf = ctx.state.csrf;
  const release = await ctx.productRelease('host');
  if (!ctx.state.user || ctx.state.user.id !== userId || ctx.state.csrf !== csrf) return;
  const holder = el('section', { class: 'host-request-list', 'aria-live': 'polite' });
  // Step 1 is the short link /get/host: typed into the VM's own browser, it
  // always downloads the newest published Host installer.
  const revision = ctx.modal(desktop ? t('hosts.126') : t('common.87'), [
    ...(desktop ? [{ label: t('hosts.27'), value: `VM ${desktop.pve_vmid} · ${desktop.name}`, readonly: true }] : []),
    ...(release ? [
      { label: desktop ? t('hosts.127') : t('hosts.128'), value: globalThis.location.origin + '/get/host', command: true, hint: t('hosts.130', { value: desktop ? t('hosts.27') : t('hosts.129'), version: release.version, filename: release.filename }) },
      { label: t('hosts.131'), value: t('hosts.132', { version: release.version }), link: release.url },
    ] : [{ label: t('hosts.133'), value: t('hosts.134'), readonly: true }]),
    { label: t('hosts.135'), value: globalThis.location.origin, command: true, hint: t('hosts.136') }, { node: holder },
  ], async () => {}, { save: t('common.31'), size: 'wide', info: true, reload: false, description: t('hosts.137') });
  const current = () => ctx.modalCurrent(revision) && ctx.state.user?.id === userId;
  const read = async () => {
    if (!current()) return;
    await ctx.load('hostRequests', { force: true });
    if (!current()) return;
    holder.replaceChildren(el('div', { class: 'panel-head' }, el('h3', {}, t('hosts.138'))), requestCards(ctx, { desktop }));
    const timer = setTimeout(read, 3000); timer.unref?.();
  };
  await read();
}

// Console state as the connected Host reports it (console-preparation).
export const consoleState = desktop => !desktop.host?.connected ? t('hosts.139') : desktop.consolePreparation ? consoleReasons[desktop.consolePreparation.reason] ?? desktop.consolePreparation.reason : t('common.17');

// Automatic Host diagnostics of one VM (host-diagnostics-v1).
const yesNo = bit => bit === true ? t('common.115') : bit === false ? t('common.116') : t('common.17');
// One fact in its own words: the yes word, the no word, or 확인 필요 when the Host did not say.
const said = (bit, yes, no) => bit === true ? yes : bit === false ? no : t('common.17');
function recoveryDiagnostics(recovery) {
  const text = code => recoveryLabels[code] ? `${recoveryLabels[code]} (${code})` : code;
  const flag = bit => bit === true ? t('common.115') : bit === false ? t('common.116') : t('hosts.140');
  const present = Object.entries(recovery.records).filter(([, value]) => value !== 'absent').map(([name, value]) => value === 'present' ? name : t('hosts.141', { name }));
  return [el('h4', {}, t('hosts.142')), facts([
    [t('hosts.143'), text(recovery.hold)], [t('hosts.144'), text(recovery.blocker)],
    [t('hosts.145'), text(recovery.path)], [t('hosts.146'), text(recovery.worker)],
    [t('hosts.147'), text(recovery.ledger)], [t('hosts.148'), text(recovery.quiescent)], [t('hosts.149'), text(recovery.retirement)],
    [t('hosts.150'), present.length ? present.join(', ') : t('common.36')],
    [t('hosts.151'), `${recovery.profilePhase} / ${flag(recovery.profileResize)}`], [t('hosts.152'), `${recovery.runtimePhase} / ${flag(recovery.runtimeCheckpoint)}`],
    [t('hosts.153'), `${flag(recovery.runtimeMatchesProfile)} / ${flag(recovery.launchMatchesProfile)}`], [t('hosts.154'), `${recovery.launchService} / ${recovery.launchWorker}`],
    [t('hosts.155'), recovery.logon === 'same' ? t('hosts.156') : recovery.logon === 'new' ? t('hosts.157') : recovery.logon],
    [t('hosts.158'), t('hosts.161', { attempts: recovery.attempts, value: recovery.budgetExhausted ? t('hosts.159') : '', value2: recovery.lastAttemptAgeMs === null ? '' : t('hosts.160', { count: Math.round(recovery.lastAttemptAgeMs / 1000) }) })],
  ], 'form-table is-pairs')];
}
export function displayPanel(ctx, desktop) {
  const entry = ctx.resource('diagnostics:' + desktop.id), value = entry?.value;
  const name = 'diagnostics:' + desktop.id;
  const collect = button(t('hosts.162'), async () => {
    collect.disabled = true;
    try { await ctx.api('/desktops/' + encodeURIComponent(desktop.id) + '/diagnostics', 'POST', {}); ctx.notice(t('hosts.163')); const timer = setTimeout(() => void ctx.load(name, { force: true }), 1500); timer.unref?.(); }
    catch (error) { ctx.notice(error.message, true); collect.disabled = false; }
  }, 'secondary', 'refresh');
  disable(collect, !value ? t('hosts.164') : !value.connected ? t('hosts.165') : !value.supported ? t('hosts.166') : null);
  const monitors = desktop.host?.capabilities?.monitors;
  const consoleText = consoleState(desktop);
  const head = { title: t('hosts.167'), actions: [collect], className: 'display-panel' };
  if (!value) return panel(head, entry?.error ? errorState(t('hosts.168'), entry.error, () => ctx.load(name, { force: true })) : loadingState(t('hosts.169')));
  const report = value.report, display = report?.display, native = report?.native;
  const bit = (b, yes, no) => display?.observed === true ? said(b, yes, no) : t('hosts.170');
  // When the server last received the Host's report, and whether the Host answers now: rows of the form table.
  const statusRows = value.receivedAt
    ? [[t('hosts.173'), value.stale ? el('span', { class: 'state-fact' }, timeTag(value.receivedAt), ' ', stateDot(t('hosts.171'), 'amber')) : el('span', {}, timeTag(value.receivedAt), ' ' + t('hosts.172'))],
      [t('hosts.diagHost'), value.connected ? t('common.95') : stateDot(t('common.23'), 'red')]]
    : [[t('hosts.173'), !value.connected ? t('hosts.174') : value.supported ? t('hosts.175') : t('hosts.176'), !value.connected ? t('hosts.177') : value.supported ? t('hosts.178') : t('hosts.179')]];
  if (!report) return panel(head, facts([...statusRows,
    [t('hosts.180'), desktop.display?.activeCount ? t('hosts.181', { activeCount: desktop.display.activeCount }) : t('common.17')],
    [t('common.68'), consoleText],
  ], 'form-table is-pairs'));
  const recentEvents = hostEntries([{ desktopId: desktop.id, diagnostics: report }]);
  // A problem reads as a dot and its word; every other value is plain text.
  const flagged = (text, tone) => tone ? stateDot(text, tone) : text;
  return panel(head,
    facts([...statusRows,
      [t('hosts.180'), display.observed === true ? t('hosts.182', { monitorCount: display.monitorCount }) : t('hosts.170')],
      [t('hosts.183'), flagged(displayReason(display.reason), display.reason ? 'amber' : '')],
      [t('hosts.184'), native.available ? nativePhases[native.phase] ?? t('common.17') : t('hosts.185')],
      native.available && native.failureReason ? [t('hosts.184a'), stateDot(displayReason(native.failureReason), 'amber')] : null,
      [t('hosts.186'), bit(display.installed, t('hosts.187'), t('hosts.188'))],
      [t('hosts.186a'), bit(display.deviceReady, t('hosts.189'), t('hosts.190'))],
      [t('hosts.191'), bit(display.owned, t('common.117'), t('common.36'))],
      [t('hosts.191a'), bit(display.activeDisplay, t('common.118'), t('common.96'))],
      [t('hosts.192'), flagged(bit(display.rebootRequired, t('hosts.193'), t('hosts.194')), display.rebootRequired ? 'red' : '')],
      [t('hosts.192a'), flagged(bit(display.recoveryRequired, t('hosts.195'), t('hosts.194')), display.recoveryRequired ? 'red' : '')],
      [t('hosts.196'), said(report.interactiveSession, t('hosts.197'), t('hosts.198'))],
      [t('hosts.196a'), said(report.consoleSession, t('hosts.199'), t('hosts.200'))],
      [t('hosts.201'), consoleText],
      [t('hosts.202'), report.version],
      [t('hosts.202a'), display.observed === true ? t('hosts.203', { count: Math.round(display.observationAgeMs / 1000) }) : t('hosts.170')],
      Array.isArray(monitors) && monitors.length ? [t('hosts.204'), monitors.map((m, i) => t('hosts.206', { value: i + 1, width: m.width, height: m.height, value2: m.primary ? t('hosts.205') : '' })).join(', ')] : null,
    ], 'form-table is-pairs'),
    el('details', { class: 'disclosure', 'data-keep-open': 'diag:' + desktop.id }, el('summary', {}, t('hosts.207')),
      el('div', { class: 'disclosure-body' },
        native.available ? facts([[t('hosts.208'), said(native.leaseActive, t('hosts.209'), t('hosts.210'))], [t('hosts.208a'), said(native.activationVerified, t('hosts.211'), t('common.119'))],
          ...(native.activation ? [[native.activationCurrent ? t('hosts.212') : t('hosts.213'), `${activationReason(native.activation.reason)}${native.activation.systemError ? t('hosts.214', { systemError: native.activation.systemError }) : ''}`],
            [t('hosts.215'), said(native.activation.attempted, t('hosts.216'), t('hosts.217'))], [t('hosts.215a'), said(native.activation.windowsVerified, t('hosts.211'), t('common.119'))]] : [])], 'form-table is-pairs') : null,
        ...(native?.recovery ? recoveryDiagnostics(native.recovery) : []),
        el('h4', {}, t('hosts.218')),
        recentEvents.length ? el('ul', { class: 'event-list' }, recentEvents.map((entry, index) => el('li', {},
          el('span', { class: 'muted' }, date(report.events[index].time)), el('strong', {}, entry.title),
          el('p', { class: 'host-event-description' }, entry.detail.join(' · ')),
          el('details', {}, el('summary', {}, t('common.120')), el('code', {}, Object.entries(report.events[index].values).map(([key, field]) => `${key}=${field}`).join(' · ')))))) : el('p', { class: 'muted' }, t('hosts.219')),
        el('p', { class: 'field-hint' }, t('hosts.220')))));
}

// What this VM's Host can provide right now (host-diagnostics `devices`): the
// feature group (공유 폴더, USB, 프린터, 사운드카드) and the
// inventory group (virtual display adapters, USB controller), each optional.
// An observation only: whether a user may use a feature is that user's
// permission on this VM.
const deviceStates = { ready: [t('hosts.221'), 'green'], check: [t('common.17'), 'amber'], foreign: [t('hosts.222'), 'amber'], unknown: [t('common.121'), ''] };
const cleanupText = { none: t('hosts.223'), removed: t('hosts.224'), pending_restart: t('hosts.225'), failed: t('hosts.226'), refused: t('hosts.227') };
const blockerText = { inventory_unavailable: t('hosts.228'), current_not_ready: t('hosts.229'), display_state_pending: t('hosts.230'), busy: t('hosts.231') };
const usbControllerRow = code => code === 'started' ? ['ready', t('hosts.232')] : code === 'none' ? ['ready', t('hosts.233')]
  : code === 'foreign' ? ['foreign', t('hosts.234')] : code === 'unknown' ? ['unknown', t('hosts.235')]
  : ['check', { stopped: t('hosts.236'), unmarked: t('hosts.237'), duplicates: t('hosts.238'), multiple: t('hosts.239') }[code]];
function displayAdapterRow(a) {
  if (!a.read) return ['unknown', t('hosts.240')];
  const left = a.legacy + a.duplicate + a.unproven + a.foreign;
  const summary = t('hosts.242', { current: a.current, left, value: a.foreign ? t('hosts.241', { foreign: a.foreign }) : '' });
  const more = el('details', { class: 'device-more' }, el('summary', {}, t('common.80')), el('p', {}, [t('hosts.243', { legacy: a.legacy, duplicate: a.duplicate, unproven: a.unproven, foreign: a.foreign }), t('hosts.244', { value: a.currentStarted ? t('hosts.189') : t('hosts.190') }), t('hosts.245', { value: cleanupText[a.cleanup] }), a.blocker ? t('hosts.246', { value: blockerText[a.blocker] }) : null].filter(Boolean).join('\n')));
  return [a.current >= 1 && a.currentStarted && !left ? 'ready' : 'check', [summary, more]];
}
export function deviceReadiness({ drives, usb, printers, audio, displayAdapters, usbController }) {
  const spooler = { disabled: t('hosts.247'), missing: t('hosts.248') }[printers?.spooler] ?? t('hosts.249');
  const usbRow = usb && !usb.supported ? ['check', t('hosts.250')] : usbController ? usbControllerRow(usbController)
    : !usb ? null : usb.controller === 'started' ? ['ready', t('hosts.251')]
    : usb.controller === 'none' ? (usb.package ? ['ready', t('hosts.252')] : ['check', t('hosts.253')])
    : usb.controller === 'stopped' ? ['check', t('hosts.236')] : usb.controller === 'foreign' ? ['foreign', t('hosts.254')] : ['unknown', t('hosts.255')];
  return [
    displayAdapters ? [t('common.122'), displayAdapterRow(displayAdapters)] : null,
    drives ? [t('common.123'), drives.winfsp ? ['ready', t('hosts.256')] : drives.webClient === 'usable' ? ['ready', t('hosts.257') + (drives.package ? t('hosts.258') : '')]
      : drives.webClient === 'unknown' ? ['unknown', t('hosts.259')] : ['check', drives.webClient === 'disabled' ? t('hosts.260') : t('hosts.261')]] : null,
    usbRow ? ['USB', usbRow] : null,
    printers ? [t('common.124'), printers.reason === '' ? ['ready', t('hosts.262')] : printers.reason === 'print_spooler_unavailable' ? ['check', spooler]
      : printers.reason === 'print_driver_unavailable' ? ['check', t('hosts.263')] : ['unknown', t('hosts.264')]] : null,
    audio ? [t('hosts.265'), audio.output === 'present' ? ['ready', t('hosts.266')] : audio.output === 'absent' ? ['check', t('hosts.267')]
      : audio.output === 'remote' ? ['unknown', t('hosts.268')] : ['unknown', t('hosts.269')]] : null,
  ].filter(Boolean);
}export function devicePanel(ctx, desktop) {
  const name = 'diagnostics:' + desktop.id, entry = ctx.resource(name), value = entry?.value, devices = value?.report?.devices;
  const head = { title: t('hosts.270'), description: t('hosts.271'), className: 'device-panel' };
  if (!value) return panel(head, entry?.error ? errorState(t('hosts.272'), entry.error, () => ctx.load(name, { force: true })) : loadingState(t('hosts.273')));
  if (!devices) return panel(head, emptyState(t('hosts.274'), !value.connected ? t('hosts.275') : t('hosts.276'), { compact: true, iconName: 'server' }));
  return panel(head, value.stale ? el('p', { class: 'panel-status' }, stateDot(t('hosts.171'), 'amber'), t('hosts.277'), timeTag(value.receivedAt)) : null,
    el('div', { class: 'device-list' }, deviceReadiness(devices).map(([label, [state, detail]]) =>
      el('div', { class: 'device-row', 'data-device': label }, el('strong', {}, label), stateDot(...deviceStates[state]), el('div', { class: 'device-detail' }, detail)))));
}

export { windowsCapable };
