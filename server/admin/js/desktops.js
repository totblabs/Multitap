// 데스크톱: every registered Host (a Proxmox VM or a local PC) in one list, the
// ones that need attention first. A row opens that desktop's window (창): what
// is wrong and its record in 기본 정보, the people who use it and their sharing
// in 사용자, its problems and records in 최근 기록, what its Host reports in
// 자세히, and power in the bar at the bottom.
import { el, icon, button, stateDot, panel, facts, emptyState, loadingState, errorState, date, relative, timeTag, sinceText, disable, tally, listHead, queryBox, queryField, queryText, board, select, detailWindow } from './dom.js';
import { osNames, powerNames, vmTags, vmPool, tagsHint, vmSearchText, messages, hostUpdateLabels } from './labels.js';
import { hostCell, hostUpdateButton, hostUpdateView, hostUpdateFailed, hostUpdateDetail, hostUpdateEligible, hostUpdateAll, hostUpdateAllLabel, hostBatchPanel, enrollmentGuide, requestCards, displayPanel, devicePanel, enrolledHost, hostConnected, hostVersion, consoleState, localPc, registeredHostIds, validHostId, desktopStatus, statusOrder, renamePc } from './hosts.js';
import { accountOwner, accountCurrent, requireAccountOwner, savedPermissions, sharingSentence, sharingName, sharingControl, saveDesktopSharing, removeAssignment, presetChoices, presetValue, assignable } from './permissions.js';
import { collect, logBoard, desktopHostList } from './timeline.js';
import { sessionHeadline, metricRow, sessionDetail } from './perf.js';
import { openLog } from './monitor.js';
import { announceFirst, announcements, openNotices } from './notices.js';
import { t } from './i18n.js';

export const vmOrder = (a, b) => Number(a.pve_vmid) - Number(b.pve_vmid);
export const osFamily = desktop => Object.hasOwn(osNames, desktop.pve_os_family) ? desktop.pve_os_family : 'unknown';
const powerKey = status => status === 'running' || status === 'stopped' ? status : 'unknown';
// "VM 101" or "로컬 PC": what a desktop is, before its name.
export const desktopKind = desktop => localPc(desktop) ? t('common.22') : `VM ${desktop.pve_vmid}`;
export const desktopLabel = desktop => `${desktopKind(desktop)} · ${desktop.name}`;
// Proxmox tags (and the pool) as small chips; read-only, managed in Proxmox.
export function tagChips(desktop) {
  const pool = vmPool(desktop), tags = vmTags(desktop);
  if (!pool && !tags.length) return null;
  return el('span', { class: 'tag-chips', title: tagsHint }, pool ? el('span', { class: 'tag-chip pool', title: t('desktops.1') }, pool) : null, ...tags.map(tag => el('span', { class: 'tag-chip' }, tag)));
}
// 운영체제 as plain text: what the desktop's record says ("Windows 11"), else the family Proxmox reports; where it
// was read from is the title.
function osText(desktop) {
  const family = osFamily(desktop), named = desktop.os && !['미확인', 'unknown'].includes(desktop.os.toLowerCase()) /* the server's stored placeholder (store.mjs), data in either language */;
  return el('span', { title: family === 'unknown' ? null : desktop.pve_os_source === 'proxmox-config' ? t('desktops.3', { value: desktop.pve_os_type || t('desktops.2') }) : t('desktops.4') }, named ? desktop.os : osNames[family]);
}

// The VM list waits for the first successful Proxmox check, and a failed
// check never presents the previous list as current.
export function vmLoadingState(ctx) {
  const s = ctx.state;
  if (!s.data.proxmox.configured) return emptyState(t('desktops.5'), t('desktops.6'), { panel: true, iconName: 'cube', action: button(t('common.10'), () => ctx.navigate('proxmox'), 'primary') });
  if (s.pveBusy) return emptyState(t('desktops.7'), t('desktops.8'), { panel: true, iconName: 'loading', role: 'status' });
  if (s.pveError) return emptyState(t('desktops.9'), s.pveError, { panel: true, iconName: 'alert', tone: 'is-error', role: 'alert', action: button(t('common.41'), ctx.syncProxmox, 'secondary', 'refresh') });
  if (!s.pveLoaded) return emptyState(t('desktops.10'), '', { panel: true, iconName: 'refresh', action: button(t('common.42'), ctx.syncProxmox, 'secondary') });
  return null;
}

const sessionsOf = (ctx, id) => (ctx.value('sessions')?.sessions ?? []).filter(session => session.desktopId === id);
const liveAccounts = (ctx, id) => [...new Set(sessionsOf(ctx, id).map(session => session.account).filter(Boolean))];
const assignedNames = (ctx, id) => ctx.state.data.assignments.filter(a => a.desktop_id === id).map(a => a.username).sort((a, b) => a.localeCompare(b, 'ko'));
// Who loses a connection, for a confirmation: named from the current session
// list, and said so when that list is unknown.
export function whoBox(ctx, names, { calm = t('common.43'), suffix = t('desktops.11') } = {}) {
  const known = !!ctx.value('sessions');
  return el('p', { class: 'confirm-who' + (names.length ? '' : ' is-calm') }, icon('user'), el('span', {}, names.length ? names.join(', ') + suffix : known ? calm : t('common.44')));
}

const powerLabels = { start: t('desktops.12'), shutdown: t('common.45'), reboot: t('common.46') };
// Starting interrupts nothing: the button itself is the request. Turning off
// and restarting end work on the desktop, so they ask once and name who is
// connected.
export function powerForm(ctx, desktop, action) {
  if (!desktop || desktop.kind === 'pc' || !powerLabels[action]) return;
  // The request to Proxmox, then its task until it ends (at most 15 looks, 1.5 s apart).
  const request = async () => {
    const owner = ctx.pageAction('power:' + desktop.id);
    const task = await ctx.api('/desktops/' + desktop.id + '/power', 'POST', { action });
    owner.check();
    ctx.notice(t('desktops.13', { name: desktop.name }));
    return { owner, task };
  };
  const follow = async ({ owner, task }) => {
    for (let attempt = 0; attempt < 15; attempt++) {
      await new Promise(resolve => setTimeout(resolve, 1500));
      owner.check();
      const result = await ctx.api('/proxmox/task', 'POST', { task: task.task, node: task.node });
      owner.check();
      if (result.status === 'stopped') {
        if (result.exitStatus !== 'OK') throw new Error(t('desktops.14') + result.exitStatus);
        ctx.notice(t('desktops.15', { name: desktop.name })); return;
      }
    }
    ctx.notice(t('desktops.16'));
  };
  if (action === 'start') { void startNow(ctx, desktop, async () => follow(await request())); return; }
  // One power request per VM at a time: the one in flight is still being followed.
  if (starting(ctx).has(desktop.id)) { ctx.notice(t('desktops.powerBusyF3'), true); return; }
  // 먼저 알리기 (js/notices.js): only while somebody is connected to this desktop.
  const live = liveAccounts(ctx, desktop.id);
  const announce = announceFirst(ctx, { audience: live.length ? { target: { kind: 'desktop', desktopIds: [desktop.id] }, people: live.length } : null, text: announcements[action === 'reboot' ? 'reboot' : 'shutdown'](desktop.name) });
  // The confirmation closes as soon as Proxmox has taken the request (it does not stand with its buttons off
  // until the task ends, which can take 20 s and more). The task is followed after it, as 켜기 follows its own; a request
  // Proxmox refuses is still said in the dialog.
  const confirmed = async () => { const started = await request(); void startNow(ctx, desktop, () => follow(started), action); };
  ctx.modal(t(action === 'reboot' ? 'desktops.rebootTitleF3' : 'desktops.shutdownTitleF3', { name: desktop.name }), [
    { node: whoBox(ctx, live) },
    { node: el('p', { class: 'modal-description' }, (live.length ? t('desktops.17') : t('desktops.18')) + (action === 'shutdown' ? t('desktops.19') : t('desktops.20'))) },
    ...(announce ? [{ node: announce.node }] : []),
  ], announce ? announce.wrap(confirmed) : confirmed, { save: powerLabels[action], danger: true, safeFocus: true });
}
// One power request per VM at a time; its buttons show the request in flight (id -> start, reboot or shutdown).
const starting = ctx => ctx.state.ui.powerStarting ??= new Map();
async function startNow(ctx, desktop, run, kind = 'start') {
  const pending = starting(ctx);
  if (pending.has(desktop.id)) return;
  const action = ctx.pageAction('power-start:' + desktop.id);
  pending.set(desktop.id, kind); ctx.render();
  try { await run(); action.check(); await ctx.refresh(); action.check(); }
  catch (error) { if (action.current() && error.code !== 'admin_session_changed') ctx.notice(error.message, true); }
  finally { if (action.current()) { pending.delete(desktop.id); if (ctx.state.user) ctx.render(); } }
}
export const startPending = (ctx, desktop) => starting(ctx).has(desktop.id);
// Why a power action is unavailable in the current state (or null).
export const powerBlocked = (desktop, action) => action === 'start' && desktop.pve_power_status === 'running' ? t('desktops.21')
  : action !== 'start' && desktop.pve_power_status === 'stopped' ? t('desktops.22') : null;
function powerButton(ctx, desktop, action, cls = 'secondary') {
  const busy = starting(ctx).get(desktop.id) ?? null;
  return disable(button(action === 'start' && busy === 'start' ? t('common.47') : powerLabels[action], () => powerForm(ctx, desktop, action), cls),
    powerBlocked(desktop, action) ?? (busy === 'start' ? t('desktops.23') : busy ? t('desktops.powerBusyF3') : null));
}
export function desktopForm(ctx, desktop) {
  ctx.modal(t('desktops.24'), [{ name: 'name', label: t('desktops.25'), value: desktop.name, max: 80 }, { name: 'os', label: t('desktops.26'), value: desktop.os, max: 80 }, { name: 'description', label: t('desktops.27'), value: desktop.description, max: 200 }],
    input => ctx.change('/desktops/' + desktop.id, 'PUT', input), { description: t('desktops.28') });
}

// One more account for this desktop. Saved at once; the new assignment starts
// with nothing shared.
export function assignDesktopUser(ctx, desktop) {
  if (ctx.state.assignmentSaving || ctx.state.pveBusy || !desktop || desktop.kind === 'pc' && !localPc(desktop)) return;
  if (!registeredHostIds(ctx)?.has(desktop.id)) { ctx.notice(messages.host_not_registered, true); return; }
  const assigned = new Set(ctx.state.data.assignments.filter(row => row.desktop_id === desktop.id).map(row => row.username));
  const users = ctx.state.data.users.filter(user => !user.disabled && !assigned.has(user.username));
  if (!users.length) { ctx.notice(t('desktops.29'), true); return; }
  let choices;
  try { choices = new Map(users.map(user => { const owner = requireAccountOwner(ctx, user); return [owner.username, owner]; })); }
  catch (error) { ctx.notice(error.message, true); return; }
  const actor = ctx.state.user, csrf = ctx.state.csrf, dialogAction = ctx.pageAction('desktop-assignment-dialog');
  ctx.modal(t('desktops.30'), [{ label: t('common.5'), value: desktopLabel(desktop), readonly: true },
    { name: 'username', label: t('common.7'), options: users.map(user => [user.username, user.username]) }], async input => {
    dialogAction.check();
    if (ctx.state.user !== actor || ctx.state.csrf !== csrf) throw new Error(t('desktops.31'));
    const selectedOwner = choices.get(input.username);
    if (!selectedOwner) throw new Error(messages.account_identity_changed);
    const user = requireAccountOwner(ctx, selectedOwner), target = ctx.desktopById(desktop.id);
    if (!target || !registeredHostIds(ctx)?.has(target.id) || target.kind === 'pc' && !localPc(target)) throw new Error(messages.host_not_registered);
    const saved = new Set(ctx.state.data.assignments.filter(row => row.username === user.username).map(row => row.desktop_id));
    const available = new Set(assignable(ctx).map(d => d.id));
    const desktopIds = [...new Set([...saved].filter(id => available.has(id)).concat(desktop.id))];
    const action = ctx.pageAction('assignment-save');
    ctx.state.assignmentSaving = true;
    try {
      const result = await ctx.api('/assignments/batch', 'PUT', { username: user.username, expectedAccountId: user.id, desktopIds });
      action.check();
      await ctx.refresh(); action.check(); ctx.notice(result.added === 0 ? t('desktops.32') : t('desktops.33', { username: user.username, name: desktop.name }));
    } finally { if (action.current()) { ctx.state.assignmentSaving = false; ctx.render(); } }
  }, { save: t('common.35'), reload: false, description: t('common.48') });
}

// 쓰는 사람: the accounts of one desktop, each with its sharing as one named
// choice. A change is saved at once (permissions.js saveDesktopSharing).
function peoplePanel(ctx, desktop) {
  const locked = ctx.state.assignmentSaving || ctx.state.pveBusy, busy = locked ? t('desktops.34') : null;
  const rows = ctx.state.data.assignments.filter(a => a.desktop_id === desktop.id).map(a => {
    const user = ctx.userByName(a.username); let owner = null;
    try { owner = accountOwner(user); } catch { /* an account without a usable identity stays read-only */ }
    return { name: a.username, user, owner, editable: !!user && !user.disabled && accountCurrent(ctx, owner) };
  }).sort((a, b) => a.name.localeCompare(b.name, 'ko'));
  const ui = ctx.state.ui, live = new Set(liveAccounts(ctx, desktop.id));
  const editable = rows.filter(row => row.editable);
  const list = rows.map(row => {
    const key = `${row.owner?.id ?? row.name}:${desktop.id}`, open = ui.sharingOpen === key && row.editable;
    const value = savedPermissions(ctx, row.owner, desktop.id);
    const toggle = () => { ui.sharingOpen = open ? null : key; ctx.render({ refocus: { key: 'share-toggle:' + key } }); };
    return el('li', { class: 'person-row' + (open ? ' is-open' : ''), 'data-person': row.name },
      el('div', { class: 'row-line' },
        el('div', { class: 'row-text' }, el('strong', {}, ctx.link(row.name, 'user', row.name, { class: 'row-link' }), live.has(row.name) ? stateDot(t('common.49'), 'green') : null, row.user?.disabled ? stateDot(t('common.25'), 'muted') : null),
          el('small', {}, t('desktops.35', { sharingName: sharingName(value) }), sharingName(value) === t('common.50') ? ` · ${sharingSentence(value)}` : null)),
        el('div', { class: 'row-actions' },
          disable(button(open ? t('common.31') : t('desktops.36'), toggle, 'quiet small', null, { 'aria-expanded': String(open), 'data-focus': 'share-toggle:' + key }), busy ?? (row.editable ? null : row.user?.disabled ? t('common.51') : messages.account_identity_changed)),
          disable(button(t('common.52'), () => removeAssignment(ctx, row.owner, desktop), 'quiet small', null, { 'aria-label': t('common.53', { name: row.name }) }), busy ?? (row.user?.disabled ? t('common.51') : row.owner && accountCurrent(ctx, row.owner) ? null : messages.account_identity_changed)))),
      open ? sharingControl(ctx, { owner: row.owner, desktop, value, locked: busy, save: next => saveDesktopSharing(ctx, desktop, [{ owner: row.owner, value: next }]) }) : null);
  });
  // The same choice for everyone here, in one atomic request.
  const everyone = editable.length > 1 ? el('div', { class: 'same-for-all', role: 'group', 'aria-label': t('desktops.37') }, el('span', {}, t('desktops.38')),
    presetChoices.map(([preset, label]) => disable(button(label, () => saveDesktopSharing(ctx, desktop, editable.map(row => ({ owner: row.owner, value: presetValue(preset, savedPermissions(ctx, row.owner, desktop.id)) })), { everyone: true }), 'quiet small', null, { 'data-same-preset': preset }), busy))) : null;
  return panel({ title: t('desktops.39'), count: rows.length, id: 'desktop-people', className: 'people-panel', actions: [disable(button(t('desktops.30'), () => assignDesktopUser(ctx, desktop), 'secondary small', 'plus'), busy)] },
    rows.length ? [el('ul', { class: 'row-list' }, list), everyone,
      el('p', { class: 'note' }, icon('info'), el('span', {}, t('desktops.40')))]
      : el('p', { class: 'panel-empty' }, t('desktops.41')));
}

// Wake-on-LAN: the wired adapters the PC's Host last reported (administrator
// only; clients learn only whether 켜기 is possible).
// 원격 켜기 of a local PC in a few words (가능 and the network card's address); each card's settings, when the Host
// reported them and that the PC must be on the server's network are its title.
function wakeFacts(wake) {
  if (!wake) return el('span', { title: t('desktops.42') }, t('desktops.wakeUnknown'));
  if (!wake.adapters.length) return el('span', { title: t('desktops.43') }, t('desktops.wakeNone'));
  const flag = (value, on, off) => value === true ? on : value === false ? off : null;
  const ready = wake.adapters.some(a => a.wakeEnabled === true && a.magicPacket === true);
  const title = [...wake.adapters.map(a => [a.mac, flag(a.wakeEnabled, t('desktops.44'), t('desktops.45')), flag(a.magicPacket, t('desktops.46'), t('desktops.47'))].filter(Boolean).join(' · ')),
    t('desktops.48', { when: relative(wake.reportedAt) }), t('desktops.49')].join('\n');
  return el('span', { class: 'wake-fact', title }, `${ready ? t('desktops.wakeReady') : t('desktops.wakeOff')} · ${wake.adapters.map(a => a.mac).join(', ')}`);
}
// 화면 보기 (Console) opens the VM's own screen in the browser Console, which
// serves the VMs assigned to the account that signs in there.
function consoleLink(ctx, desktop, cls = 'primary') {
  const mine = desktop.kind !== 'pc' && ctx.state.data.assignments.some(a => a.desktop_id === desktop.id && a.username === ctx.state.user?.username);
  return mine ? el('a', { class: 'button ' + cls, href: '/console/' + encodeURIComponent(desktop.id), target: '_blank', rel: 'noopener' }, t('desktops.50')) : null;
}

// A problem the operator has to act on, as one plain bordered line above the form table: nobody can connect
// (with why), or the last Host update did not install (with 다시 시도). Nothing when all is well: the form table
// says the state. The action that fixes a desktop nobody can reach is the main button of the window's bar.
const silentViewer = (ctx, desktop, status) => status.key === 'bad' && !['reboot', 'recovery'].includes(status.cause) ? consoleLink(ctx, desktop) : null;
function problemLines(ctx, desktop, status) {
  const line = (title, text, action = null, extra = {}) => el('div', { class: 'dt-alert', role: 'alert', ...extra }, el('strong', {}, title), text ? el('span', {}, text) : null, action);
  const lines = [];
  if (status.key === 'bad') {
    const host = status.state.host;
    lines.push(status.cause === 'reboot' ? line(t('common.54'), t('desktops.51'))
      : status.cause === 'recovery' ? line(t('common.55'), t('desktops.53'))
        : line(t('desktops.55'), t('desktops.57', { value: host?.lastSeenAt ? t('desktops.56', { relative: relative(host.lastSeenAt) }) : '' })));
  }
  const view = hostUpdateView(ctx, desktop);
  if (hostUpdateFailed(view)) {
    const operation = view.operation, detail = hostUpdateDetail(operation);
    lines.push(line(t('desktops.updateFailed', { label: hostUpdateLabels[operation.status] ?? operation.status, version: operation.targetVersion ?? '' }), detail.split('\n')[0], hostUpdateButton(ctx, desktop, 'secondary small'), { title: detail || null, 'data-problem': 'host-update' }));
  }
  return lines.length ? el('div', { class: 'dt-alerts' }, lines) : null;
}
// The bar at the bottom of the window, plain buttons: 삭제 first, 표시 정보 수정 (이름 변경 for a local PC), a Host
// update that waits, power for a VM (what its state asks for is the main button) and a notice to whoever is
// connected. A local PC is turned on and off by its users in the client.
function detailActions(ctx, desktop, status) {
  // 공지 보내기 (js/notices.js): to the people connected to this desktop now.
  const announce = liveAccounts(ctx, desktop.id).length ? button(t('common.59'), () => openNotices(ctx, { target: 'desktop', desktop: desktop.id }), 'secondary') : null;
  const remove = deleteButton(ctx, desktop);
  const edit = localPc(desktop) ? button(t('desktops.82'), () => renamePc(ctx, desktop), 'secondary') : button(t('desktops.24'), () => desktopForm(ctx, desktop), 'secondary');
  // A failed update offers 다시 시도 in its line above; any other update is here.
  const update = hostUpdateFailed(hostUpdateView(ctx, desktop)) ? null : hostUpdateButton(ctx, desktop, 'secondary');
  if (localPc(desktop)) return [remove, edit, update, announce];
  const power = desktop.pve_power_status, viewer = silentViewer(ctx, desktop, status);
  const asked = status.key === 'off' ? 'start' : status.key === 'bad' && !viewer ? 'reboot' : null;
  const actions = power === 'stopped' ? ['start'] : power === 'running' ? ['reboot', 'shutdown'] : ['start', 'reboot', 'shutdown'];
  return [remove, edit, update, viewer, ...actions.map(action => powerButton(ctx, desktop, action, action === asked ? 'primary' : 'secondary')), announce];
}
// 삭제: one desktop from its window's bar, or the checked ones of the list (선택 삭제).
// Each goes through the existing DELETE /desktops/:id/host with its own expectedHostId, one by one, and
// re-checks its registration right before its own call. A desktop someone is connected to is left out
// (the server refuses it with host_registration_busy too). Typing confirms: the name for one desktop,
// the word 삭제 (delete) for several.
export function deleteDesktops(ctx, chosen) {
  const ids = registeredHostIds(ctx);
  const entries = chosen.map(desktop => ({ desktop, hostId: enrolledHost(ctx, desktop)?.hostId }));
  if (!entries.length || entries.some(({ desktop, hostId }) => !ids?.has(desktop.id) || !validHostId(hostId))) { ctx.notice(messages.host_identity_required, true); return; }
  const owner = ctx.pageAction('desktop-delete:' + entries.map(({ desktop }) => desktop.id).join(','));
  const busy = entries.filter(({ desktop }) => liveAccounts(ctx, desktop.id).length), targets = entries.filter(entry => !busy.includes(entry));
  const names = list => list.map(({ desktop }) => desktop.name);
  const shortList = list => list.length > 5 ? t('desktops.deleteNamesMore', { names: names(list.slice(0, 5)).join(', '), more: list.length - 5 }) : names(list).join(', ');
  const assigned = targets.reduce((sum, { desktop }) => sum + assignedNames(ctx, desktop.id).length, 0);
  const pcs = targets.filter(({ desktop }) => localPc(desktop)).length;
  const lines = [
    targets.length === 1 ? t('desktops.deleteOne', { name: targets[0].desktop.name }) : targets.length ? t('desktops.deleteMany', { count: targets.length, names: shortList(targets) }) : null,
    targets.length ? (assigned ? t('desktops.deleteAssigned', { assigned }) : t('desktops.deleteNoneAssigned')) : null,
    targets.length ? (pcs === targets.length ? t('desktops.deleteKeepsPc') : pcs ? t('desktops.deleteKeepsBoth') : t('desktops.deleteKeepsVm')) : null,
    targets.length ? t('desktops.deleteHostStays') : null,
    busy.length ? t('desktops.deleteBusy', { names: shortList(busy) }) : null].filter(Boolean);
  const single = targets.length === 1;
  const confirmText = targets.length ? { expected: single ? targets[0].desktop.name : t('desktops.deleteWord'), label: single ? t('desktops.deleteTypeName', { name: targets[0].desktop.name }) : t('desktops.deleteTypeWord') } : { blocked: true };
  let running = false;
  ctx.confirmAction(t('desktops.deleteTitle'), lines.join('\n'), async () => {
    owner.check();
    if (running || !targets.length) return;
    running = true;
    const deleted = [], skipped = [];
    let failure = null, removed = 0;
    try {
      for (const { desktop, hostId } of targets) {
        if (!registeredHostIds(ctx)?.has(desktop.id) || enrolledHost(ctx, { id: desktop.id })?.hostId !== hostId) { failure = { desktop, message: messages.host_registration_changed, code: 'host_registration_changed' }; break; }
        try {
          const result = await ctx.api('/desktops/' + encodeURIComponent(desktop.id) + '/host', 'DELETE', { expectedHostId: hostId, confirm: true });
          owner.check();
          deleted.push(desktop);
          if (Number.isSafeInteger(result?.removedAssignments) && result.removedAssignments > 0) removed += result.removedAssignments;
        } catch (error) {
          owner.check();
          if (error?.code === 'host_registration_busy') { skipped.push(desktop); continue; }
          failure = { desktop, message: error.message, code: error.code }; break;
        }
      }
    } finally { running = false; }
    // Nothing was deleted: the dialog says why and stays, as before.
    if (!deleted.length && failure) throw Object.assign(new Error(failure.message), { code: failure.code });
    ctx.state.ui.desktops && (ctx.state.ui.desktops.selected = []);
    if (deleted.length) { await ctx.load('hosts', { force: true }); owner.check(); await ctx.refresh(); owner.check(); }
    if (ctx.state.route.page === 'desktop' && deleted.some(desktop => desktop.id === ctx.state.route.id)) ctx.navigate('desktops');
    const rest = failure ? targets.slice(targets.findIndex(({ desktop }) => desktop.id === failure.desktop.id) + 1).map(({ desktop }) => desktop) : [];
    const report = [
      deleted.length === 1 && targets.length === 1 ? t('desktops.deletedOne', { name: deleted[0].name }) : deleted.length ? t('desktops.deletedMany', { names: deleted.map(d => d.name).join(', ') }) : null,
      removed ? t('desktops.deletedAssignments', { removed }) : null,
      ...skipped.map(desktop => t('desktops.deleteSkippedBusy', { name: desktop.name })),
      failure ? t('desktops.deleteFailed', { name: failure.desktop.name, message: failure.message }) : null,
      rest.length ? t('desktops.deleteNotTried', { names: rest.map(d => d.name).join(', ') }) : null].filter(Boolean);
    ctx.notice(report.join('\n'), !!(failure || skipped.length));
  }, t('common.deleteAction'), true, { confirmText });
}

// 삭제 in the window's bar. It cannot be pressed while the registration cannot be verified.
function deleteButton(ctx, desktop) {
  return disable(button(t('common.deleteAction'), () => deleteDesktops(ctx, [desktop]), 'danger dt-delete', null, { 'data-focus': 'desktop-delete' }),
    !registeredHostIds(ctx)?.has(desktop.id) || !validHostId(enrolledHost(ctx, desktop)?.hostId) ? messages.host_identity_required : null);
}
// 전원 in the form table: 켜짐, 꺼짐 or 확인 못 함 as Proxmox last reported it (when, in the title). Who a power action
// would cut off is said where it is asked (powerForm). A local PC is turned on and off from the client.
function powerFact(ctx, desktop) {
  if (localPc(desktop)) return [t('desktops.75'), el('span', { title: t('desktops.76') }, t('desktops.powerByClient'))];
  const checked = ctx.state.data.proxmox.checkedAt, power = desktop.pve_power_status;
  return [t('desktops.75'), el('span', { class: 'power-fact', 'data-power': powerKey(power), title: [checked ? t('desktops.81', { relative: relative(checked) }) : null, power === 'running' || power === 'stopped' ? null : t('desktops.80')].filter(Boolean).join('\n') || null }, powerNames[powerKey(power)])];
}
// 상태 in the form table: one dot and its word; a desktop nobody can reach says why after it in a few red words, as
// the list does (when it last answered is the Host 응답 row).
const stateFact = status => {
  const why = status.key !== 'bad' ? null : status.cause === 'silent' ? t('common.23') : status.why;
  return [t('common.61'), el('span', { class: 'state-fact' }, stateDot(status.label, status.tone), why ? el('span', { class: 'state-why', title: status.why }, why) : null)];
};
// 지금 접속 중: who, and for how long (from the client's reports).
function liveFact(ctx, desktop, status) {
  if (status.key !== 'live') return null;
  const events = ctx.value('clientDiagnostics:' + desktop.id)?.events ?? [], names = liveAccounts(ctx, desktop.id);
  const started = status.live.map(session => events.filter(e => e.sessionId === session.sessionId && ['streamStart', 'streamReady'].includes(e.event)).map(e => Date.parse(e.time)).filter(Number.isFinite)).flat();
  return [t('common.69'), [names.length ? names.join(', ') : t('common.71'), started.length ? ' · ' + sinceText(Math.min(...started)) : null, status.live.every(session => session.ready) ? null : (started.length ? ' (' + t('desktops.62') + ')' : ' · ' + t('desktops.62'))]];
}

// 기본 정보: a problem to act on (only then), and the desktop's record as a form table of plain values.
function infoTab(ctx, desktop, status) {
  const vm = !localPc(desktop), host = enrolledHost(ctx, desktop), connected = hostConnected(ctx, desktop), modes = desktop.connectionModes ?? {};
  return [problemLines(ctx, desktop, status),
    el('div', { class: 'desktop-host-summary' }, facts([
      [t('common.60'), desktop.name],
      stateFact(status),
      [t('common.62'), desktopKind(desktop)],
      ...(vm ? [[t('common.63'), desktop.pve_node || '—'], [t('desktops.26'), osText(desktop)]] : []),
      powerFact(ctx, desktop),
      ['Host', hostCell(ctx, desktop)],
      [t('desktops.83'), connected ? t('common.65') : host?.lastSeenAt ? timeTag(host.lastSeenAt) : '—'],
      [t('common.66'), desktop.host?.hostname ?? host?.hostname ?? '—'],
      host?.enrolledAt ? [t('desktops.84'), date(host.enrolledAt)] : null,
      // What a desktop that answers can do now; one that does not answer says so above.
      connected ? [t('common.67'), modes.stream ? t('desktops.85') : t('desktops.86')] : null,
      ...(vm ? [connected ? [t('common.68'), consoleState(desktop)] : null] : []),
      liveFact(ctx, desktop, status),
      vm && tagChips(desktop) ? [t('desktops.87'), tagChips(desktop)] : null,
    ], 'form-table is-pairs'))];
}
// 사용자: who may use the desktop with their sharing, and who is connected now.
function usersTab(ctx, desktop) {
  const live = sessionsOf(ctx, desktop.id), sessionsEntry = ctx.resource('sessions');
  const sessionPanel = live.length || sessionsEntry?.error ? panel({ title: t('common.69'), count: live.length, level: 'h3' },
    !sessionsEntry?.value ? errorState(t('common.70'), sessionsEntry.error) :
      el('ul', { class: 'session-list' }, live.map(s => el('li', {}, icon('monitor'), el('span', {}, s.account ? ctx.link(s.account, 'user', s.account) : t('common.71')), stateDot(s.ready ? t('common.49') : s.attached ? t('common.72') : t('desktops.88'), s.ready ? 'green' : 'amber'))))) : null;
  return [peoplePanel(ctx, desktop), sessionPanel];
}
// 최근 기록: the problems of the last week first, then everything recorded about this desktop.
function recordsTab(ctx, desktop) {
  const clientEntry = ctx.resource('clientDiagnostics:' + desktop.id), week = Date.now() - 7 * 86400000;
  const entries = collect(ctx, { clientRows: clientEntry?.value?.events ?? [], hostList: desktopHostList(ctx, desktop.id) }).filter(e => e.desktopId === desktop.id && !e.stats);
  const problems = entries.filter(e => e.failure && e.time >= week);
  return [panel({ title: t('desktops.89'), className: 'dt-problems', actions: [button(t('common.73'), () => openLog(ctx, { vm: desktop.id }), 'quiet small')] },
      !clientEntry?.value && !clientEntry?.error ? loadingState() :
        problems.length ? logBoard(ctx, problems, { limit: 3, showVm: false, inline: 'dt-problems' }) : el('p', { class: 'panel-empty' }, t('desktops.90'))),
    panel({ title: t('desktops.91'), level: 'h3', description: t('desktops.92') },
      clientEntry?.error ? el('p', { class: 'is-error' }, t('desktops.93') + clientEntry.error) : null,
      logBoard(ctx, entries, { limit: 150, showVm: false, cause: false, inline: 'dt-records', emptyTitle: t('desktops.94') }))];
}
// 자세히: what the Host reports (the virtual display, device support, streaming
// quality). Deleting the desktop is 삭제 in the window's bar.
function moreTab(ctx, desktop) {
  const vm = !localPc(desktop);
  const stats = (ctx.value('streamStats')?.sessions ?? []).filter(s => s.desktopId === desktop.id).slice(0, 3), statsEntry = ctx.resource('streamStats');
  const perfPanel = !vm ? null : stats.length || statsEntry?.error ? panel({ title: t('desktops.95'), level: 'h3', actions: [ctx.link(t('common.73'), 'log', 'quality', { class: 'button quiet small' })] },
    !statsEntry?.value ? errorState(t('desktops.96'), statsEntry.error) :
      el('div', { class: 'perf-list' }, stats.map((session, index) => { const head = sessionHeadline(session, desktop);
        return el('details', { class: 'perf-session', 'data-keep-open': 'perf:' + (session.sessionId ?? index), open: index === 0 }, el('summary', {}, el('span', { class: 'perf-title' }, el('strong', {}, session.account ?? t('common.71')), el('small', {}, head.meta)), metricRow(head.s)), sessionDetail(session)); })))
    : el('p', { class: 'field-hint' }, t('desktops.97'));
  return [vm ? displayPanel(ctx, desktop) : facts([[t('desktops.98'), wakeFacts(desktop.wake)], [t('desktops.99'), el('span', { title: t('desktops.100') }, t('desktops.consoleNone'))]], 'form-table'),
    devicePanel(ctx, desktop), perfPanel];
}
// The window of one desktop (dom.js detailWindow): its tabs and the actions of its bar.
function desktopWindow(ctx, desktop, status) {
  const vm = !localPc(desktop), family = osFamily(desktop);
  // What the desktop's state asks for (켜기, 다시 시작) is the dark button on the right, before 닫기; the rest stand on the left.
  const acts = detailActions(ctx, desktop, status).filter(Boolean), main = acts.find(node => node.classList?.contains('primary')) ?? null;
  return detailWindow(ctx, { key: 'desktop:' + desktop.id, kind: t('desktops.104'), title: desktop.name, className: 'dt-main', headClass: 'dt-head', focusKey: 'vm-pick-' + desktop.id, close: () => ctx.navigate('desktops'),
    sub: [desktopKind(desktop), vm ? desktop.pve_node || t('common.77') : null, vm && family !== 'unknown' ? osNames[family] : null].filter(Boolean).join(' · '),
    badges: [stateDot(status.label, status.tone), ctx.state.pveBusy && vm ? el('small', { class: 'win-busy', role: 'status' }, t('desktops.105')) : null],
    tabs: [{ key: 'info', label: t('common.78'), nodes: infoTab(ctx, desktop, status) }, { key: 'users', label: t('common.7'), count: assignedNames(ctx, desktop.id).length, nodes: usersTab(ctx, desktop) },
      { key: 'records', label: t('common.79'), nodes: recordsTab(ctx, desktop) }, { key: 'more', label: t('common.80'), nodes: moreTab(ctx, desktop) }],
    actions: acts.filter(node => node !== main), main });
}

const filters = ['all', 'bad', 'live', 'ok', 'off', 'update'];
// What the list shows: one state (or the desktops with a Host update), a search text, and whether it is grouped.
const listUi = ctx => {
  const ui = ctx.state.ui.desktops ??= { query: '', filter: 'all', byNode: false };
  if (!filters.includes(ui.filter)) ui.filter = 'all';
  return ui;
};
// The conditions as they stand in the 조회 조건 box: what was changed there and not applied with 조회 yet, as
// long as the list still shows what they were changed from (a link from another page may have set a filter).
function listDraft(ctx) {
  const ui = listUi(ctx), of = ui.filter + '\n' + ui.query;
  return ui.draft?.of === of ? ui.draft : { filter: ui.filter, query: ui.query, of };
}
function registrationLoadingState(ctx) {
  const entry = ctx.resource('hosts');
  const error = entry?.error || (entry?.value ? t('common.81') : '');
  return error ? errorState(t('common.82'), error, () => ctx.load('hosts', { force: true })) : loadingState(t('common.83'));
}
// Registered Hosts only: an unregistered VM or an unapproved PC is never listed.
function listBase(ctx) {
  const ids = registeredHostIds(ctx);
  return ctx.state.data.desktops.filter(d => ids?.has(d.id) && (localPc(d) || d.kind !== 'pc'));
}
const needsUpdate = (ctx, d) => !!hostUpdateView(ctx, d)?.available;
// Problems first, then connected, available and off; VMs by number, then PCs by name.
function listRows(ctx) {
  const ui = listUi(ctx), query = ui.query.trim().toLowerCase();
  return listBase(ctx).map(d => ({ d, status: desktopStatus(ctx, d) }))
    .filter(({ d, status }) => (ui.filter === 'all' || (ui.filter === 'update' ? needsUpdate(ctx, d) : status.key === ui.filter))
      && (!query || `${localPc(d) ? t('desktops.106') : 'vm '}${vmSearchText(d)} ${d.host?.hostname ?? status.state.host?.hostname ?? ''} ${assignedNames(ctx, d.id).join(' ')}`.toLowerCase().includes(query)))
    .sort((a, b) => statusOrder[a.status.key] - statusOrder[b.status.key] || Number(localPc(a.d)) - Number(localPc(b.d)) || (localPc(a.d) ? a.d.name.localeCompare(b.d.name, 'ko') : vmOrder(a.d, b.d)));
}
// The desktop whose window is open (#/desktops/<id>): a registered one only.
function openDesktop(ctx, route) {
  if (!ctx.state.data || !route?.id) return null;
  const ids = registeredHostIds(ctx), desktop = ctx.desktopById(route.id);
  return ids?.has(route.id) && desktop && (desktop.kind !== 'pc' || localPc(desktop)) ? desktop : null;
}
// The list reads the registrations, the connections and the requests; an open window also reads its desktop's reports.
export function workspaceNeeds(ctx, route) {
  const d = openDesktop(ctx, route);
  return ['hosts', 'sessions', 'hostRequests', ...(d ? [`diagnostics:${d.id}`, `clientDiagnostics:${d.id}`, 'streamStats'] : [])];
}
export function list(ctx) { return page(ctx, {}); }
export function detail(ctx, route) { return page(ctx, route); }

const columns = [{ label: t('common.84'), cls: 'col-num' }, { label: t('common.61'), cls: 'col-state is-state' }, { label: t('common.60'), cls: 'col-name is-name' }, { label: t('common.62'), cls: 'col-kind' }, { label: t('common.63'), cls: 'col-node' },
  { label: t('common.7'), cls: 'col-users' }, { label: 'Host', cls: 'col-host is-mono' }, { label: t('desktops.83'), cls: 'col-seen' }, { label: t('common.85'), cls: 'col-manage' }];
function page(ctx, route) {
  const ui = listUi(ctx), ids = registeredHostIds(ctx), enrolled = listBase(ctx);
  const add = () => enrollmentGuide(ctx, null);
  const refresh = disable(button(ctx.state.pveBusy ? t('common.86') : t('common.42'), ctx.syncProxmox, 'secondary', 'refresh'), ctx.state.pveBusy ? t('desktops.107') : null);
  const addButton = button(t('common.87'), add, 'primary', 'plus');
  // The page's actions stand above the list; while there is no list to stand above, in the page's header.
  const title = { title: t('common.5') };
  const header = ctx.pageHeader(title), bare = () => ctx.pageHeader({ ...title, actions: [refresh, addButton] });
  // The list waits for the registrations.
  if (!ids) return [bare(), registrationLoadingState(ctx)];
  const all = enrolled.map(d => ({ d, status: desktopStatus(ctx, d) })), count = key => all.filter(({ status }) => status.key === key).length;
  const requestsEntry = ctx.resource('hostRequests'), waiting = (requestsEntry?.value?.requests ?? []).filter(request => request.status === 'pending');
  // Requests wait here whatever else is on the page: approving one is how a desktop enters the list.
  const requestsPanel = waiting.length || requestsEntry?.error ? panel({ title: t('common.88'), count: waiting.length || null, className: 'requests-panel', description: t('desktops.109') }, requestCards(ctx)) : null;
  const pending = vmLoadingState(ctx), desktop = openDesktop(ctx, route);
  if (pending && !enrolled.length && (!ctx.state.data.desktops.some(localPc) || route.id && !localPc(desktop))) return [bare(), requestsPanel, pending];
  // An address of a desktop that is not on the list says so instead of showing the list.
  if (route.id && !desktop) return [bare(), requestsPanel, emptyState(t('desktops.110'), t('desktops.111'), { panel: true, iconName: 'monitor', action: ctx.link(t('desktops.112'), 'desktops', null, { class: 'button secondary' }) })];
  const rows = listRows(ctx), draft = listDraft(ctx);
  // 선택 삭제: only rows the list shows now stay chosen (a filter or a search drops the others).
  const shownIds = new Set(rows.map(({ d }) => d.id));
  ui.selected = (Array.isArray(ui.selected) ? ui.selected : []).filter(id => shownIds.has(id));
  const chosen = new Set(ui.selected);
  const choose = (list, on, refocus) => { const next = new Set(ui.selected); for (const id of list) if (on) next.add(id); else next.delete(id); ui.selected = rows.map(({ d }) => d.id).filter(id => next.has(id)); ctx.render({ refocus: { key: refocus } }); };
  const eligible = hostUpdateEligible(ctx), latest = ctx.state.data.hostUpdates?.latest;
  if (Object.values(ctx.state.data.hostUpdates?.desktops ?? {}).some(view => ['waiting', 'dispatched', 'accepted', 'downloading', 'verifying', 'installing'].includes(view.operation?.status))) ctx.scheduleHostUpdatePoll();
  // 조회 조건: the state and the search text. They change the list when 조회 is pressed.
  const change = values => { ui.draft = { ...listDraft(ctx), ...values }; };
  const apply = (refocus = { key: 'query-apply' }) => { const next = listDraft(ctx); ui.filter = next.filter; ui.query = next.query; ui.draft = null; ctx.render({ refocus }); };
  const clear = () => { ui.query = ''; ui.filter = 'all'; ui.draft = null; ctx.render(); };
  const states = [['all', t('desktops.113', { enrolledCount: enrolled.length })], ['bad', t('desktops.114', { count: count('bad') })], ['live', t('desktops.115', { count: count('live') })], ['ok', t('desktops.116', { count: count('ok') })], ['off', t('desktops.117', { count: count('off') })],
    ...(eligible.length || [ui.filter, draft.filter].includes('update') ? [['update', t('desktops.118', { eligibleCount: eligible.length })]] : [])];
  const query = queryBox(t('desktops.120'), [
    queryField(t('common.61'), select(t('common.61'), states, draft.filter, value => change({ filter: value }), { attrs: { 'data-focus': 'desktops-state' } })),
    queryField(t('common.89'), queryText({ key: 'desktops-search', value: draft.query, placeholder: t('desktops.121'), onInput: value => change({ query: value }), onApply: apply }), 'is-search')], apply, clear);
  // One row of the board. The name is the link that opens the window; the whole row and 상세 open it too.
  const item = ({ d, status }, index) => {
    const users = assignedNames(ctx, d.id), where = localPc(d) ? t('common.22') : `VM ${d.pve_vmid} (${d.pve_node || t('common.77')})`;
    const people = status.key === 'live' && liveAccounts(ctx, d.id).length ? liveAccounts(ctx, d.id) : users;
    // The small line under the name (a phone shows it) always says what the desktop is and whose it is; why it
    // cannot be reached stands once, beside it (.vm-list-why). Read aloud, the row still says the reason.
    const sub = `${where} · ${people.length ? people.join(', ') : t('desktops.122')}`, said = status.key === 'bad' ? status.why : sub;
    const host = status.state.host, seen = host?.lastSeenAt ?? d.host?.lastSeenAt, why = status.cause === 'silent' ? t('common.23') : status.why ?? '';
    const open = () => ctx.navigate('desktop', d.id);
    // The checkbox chooses the row for 선택 삭제; pressing it (or its cell) does not open the window.
    const pick = el('input', { type: 'checkbox', class: 'row-check', 'aria-label': t('desktops.selectOne', { name: d.name }), checked: chosen.has(d.id), 'data-focus': 'pick:' + d.id, onchange: event => choose([d.id], event.target.checked === true, 'pick:' + d.id) });
    return { attrs: { class: 'is-pick' + (desktop?.id === d.id ? ' is-on' : '') + (chosen.has(d.id) ? ' is-chosen' : ''), 'data-status': status.key, onclick: event => { if (!event.target?.closest?.('a,button,input,label,.col-check')) open(); } }, cells: [
      pick, String(index + 1), stateDot(status.label, status.tone),
      // Read as one sentence: the headings above are for the eye only.
      [ctx.link(el('span', { class: 'vm-list-name' }, el('strong', {}, d.name), el('small', {}, sub)), 'desktop', d.id, { class: 'vm-list-item', 'aria-label': `${d.name} · ${status.label} · ${said}${needsUpdate(ctx, d) ? t('desktops.123') : ''}`, 'aria-current': desktop?.id === d.id ? 'true' : null, 'data-focus': 'vm-pick-' + d.id, 'data-status': status.key }),
        why ? el('span', { class: 'vm-list-why' }, why) : null],
      desktopKind(d), localPc(d) ? '—' : d.pve_node || '—',
      people.length ? people.join(', ') : '—',
      [hostVersion(ctx, d) ?? '—', needsUpdate(ctx, d) ? el('span', { class: 'vm-list-update', title: t('common.90') }, t('common.8')) : null],
      hostConnected(ctx, d) ? t('common.65') : seen ? timeTag(seen) : '—',
      button(t('common.91'), open, 'secondary small', null, { 'aria-label': t('desktops.124', { name: d.name }) })] };
  };
  const numbered = rows.map((entry, index) => ({ entry, index }));
  // Each table's heading checkbox chooses (or clears) the rows of that table, as they are shown.
  const table = (list, key = 'all') => {
    const ids = list.map(({ entry }) => entry.d.id), all = ids.length > 0 && ids.every(id => chosen.has(id));
    const head = el('input', { type: 'checkbox', class: 'row-check', 'aria-label': t('desktops.selectAll'), checked: all, 'data-focus': 'pick-all:' + key, onchange: event => choose(ids, event.target.checked === true, 'pick-all:' + key) });
    if (!all && ids.some(id => chosen.has(id))) head.indeterminate = true;
    return board([{ label: t('desktops.selectColumn'), head, cls: 'col-check' }, ...columns], list.map(({ entry, index }) => item(entry, index)), { className: 'vm-table' });
  };
  const nodeOf = d => localPc(d) ? null : (typeof d.pve_node === 'string' && d.pve_node.trim()) || '';
  // Grouped, the numbers follow the order on the page.
  let shown = 0;
  const groups = () => [...new Set(rows.map(({ d }) => nodeOf(d)))].sort((a, b) => (a === null) - (b === null) || String(a).localeCompare(String(b), 'ko', { numeric: true })).map(node => {
    const group = rows.filter(({ d }) => nodeOf(d) === node).map(entry => ({ entry, index: shown++ }));
    return el('section', { class: 'vm-list-group', 'aria-label': node === null ? t('common.22') : `Proxmox ${node || t('common.77')}` },
      el('h3', { class: 'vm-list-group-label' }, el('span', {}, node === null ? t('desktops.125') : `Proxmox ${node || t('common.77')}`), el('span', {}, t('desktops.126', { groupCount: group.length, filter: group.filter(({ entry }) => entry.status.key === 'bad').length }))), table(group, node === null ? 'pc' : 'node:' + node));
  });
  const narrowed = rows.length !== enrolled.length;
  const deleteSelected = disable(button(chosen.size ? t('desktops.deleteSelectedCount', { count: chosen.size }) : t('desktops.deleteSelected'), () => deleteDesktops(ctx, rows.filter(({ d }) => chosen.has(d.id)).map(({ d }) => d)), 'danger', 'trash', { 'data-focus': 'desktops-delete-selected' }),
    chosen.size ? null : t('desktops.deleteSelectedHint'));
  const head = listHead(tally([[t('common.92'), enrolled.length, t('common.93')], [t('common.94'), count('bad'), '', count('bad') ? 'bad' : ''], [t('common.49'), count('live')], [t('common.95'), count('ok')], [t('common.96'), count('off')]],
      narrowed ? el('small', { class: 'vm-list-caption' }, t('common.97'), el('span', {}, t('desktops.127', { rowsCount: rows.length, enrolledCount: enrolled.length }))) : enrolled.length ? el('small', {}, t('desktops.128')) : null),
    [el('label', { class: 'switch-label' }, t('desktops.129'), el('input', { type: 'checkbox', role: 'switch', class: 'switch-input', checked: ui.byNode, 'data-focus': 'desktops-by-node', onchange: event => { ui.byNode = event.target.checked === true; ctx.render(); } }), el('span', { class: 'switch-track', 'aria-hidden': 'true' })),
      deleteSelected, refresh, addButton]);
  const listing = el('section', { class: 'vm-list', 'aria-label': t('desktops.112') }, head,
    el('div', { class: 'vm-list-items' },
      rows.length ? (ui.byNode ? groups() : table(numbered))
      : !enrolled.length ? emptyState(t('desktops.130'), t('desktops.131'), { panel: true, iconName: 'monitor', action: button(t('common.98'), add, 'quiet') })
      : board([{ label: t('desktops.selectColumn'), cls: 'col-check' }, ...columns], [], { className: 'vm-table', empty: emptyState(t('desktops.132'), t('desktops.133'), { compact: true, iconName: 'search', action: button(t('common.99'), clear, 'quiet') }) })));
  const updateBar = ui.filter === 'update' && eligible.length && latest ? el('div', { class: 'update-bar alert-strip' }, el('span', {}, t('desktops.134', { version: latest.version })), button(hostUpdateAllLabel(eligible.length), () => hostUpdateAll(ctx), 'primary small', 'download')) : null;
  const batch = ctx.state.ui.hostBatch ? hostBatchPanel(ctx, ctx.state.ui.hostBatch, () => { ctx.state.ui.hostBatch = null; ctx.render(); }) : null;
  return [header, enrolled.some(d => d.kind !== 'pc') ? pending : null, query, requestsPanel, updateBar, batch, listing,
    desktop ? desktopWindow(ctx, desktop, all.find(entry => entry.d.id === desktop.id).status) : null];
}
