// Sharing (clipboard and devices) belongs to each assignment (user × desktop)
// and is denied by default. It is shown as one named choice per assignment:
// 기본 (the server's default for a new assignment: nothing shared), 글·그림 복사,
// 모두 허용, or 직접 설정 when the switches match none of them. A change is
// saved at once; only turning something off for someone who is connected asks
// first. There is no sound card switch here: the stored flag is kept as it
// is and not shown (keepHidden).
import { el, icon, button, disable } from './dom.js';
import { messages } from './labels.js';
import { localPc, registeredHostIds } from './hosts.js';
import { t } from './i18n.js';

export const clipboardKeys = ['textToRemote', 'textToLocal', 'filesToRemote', 'filesToLocal'];
// [suffix of the stored key, label]: "PC" is the computer in front of the user.
export const clipboardDirections = [['ToRemote', t('permissions.1')], ['ToLocal', t('permissions.2')]];
// [stored key, name, what the user gets, the fuller note].
export const deviceItems = [
  ['drives', t('common.123'), t('permissions.3'), t('permissions.4')],
  ['printers', t('common.124'), t('permissions.5'), t('permissions.6')],
  ['usbDevices', t('permissions.7'), t('permissions.8'), t('permissions.9')],
  ['smartCards', t('permissions.10'), t('permissions.11'), t('permissions.12')],
  ['serialPorts', t('permissions.15'), t('permissions.16'), t('permissions.12')],
];
const rareDeviceKeys = new Set(['smartCards', 'serialPorts']);
// Exact, complete permission values; anything absent or not true is denied.
export const exactPermissions = value => ({ clipboardPolicy: Object.fromEntries(clipboardKeys.map(key => [key, value?.clipboardPolicy?.[key] === true])), devicePolicy: Object.fromEntries(deviceItems.map(([key]) => [key, value?.devicePolicy?.[key] === true])), soundCardAdd: value?.soundCardAdd === true });
// `value` with what the console no longer edits taken from `base`: the
// retired sound-card flag.
export const keepHidden = (value, base) => exactPermissions({ ...value, soundCardAdd: base.soundCardAdd });
// `shown` compares only what the console shows.
export const samePermissions = (a, b, { shown = false } = {}) => clipboardKeys.every(key => a.clipboardPolicy[key] === b.clipboardPolicy[key]) && deviceItems.every(([key]) => a.devicePolicy[key] === b.devicePolicy[key]) && (shown || a.soundCardAdd === b.soundCardAdd);

// The named choices. 기본 is what the server gives a new assignment: every
// shown item denied (server/src/store.mjs replaceVmAssignments).
export const presetChoices = [['default', t('permissions.17')], ['clipboard', t('permissions.18')], ['all', t('permissions.19')]];
export function presetValue(preset, base) {
  const all = preset === 'all', text = all || preset === 'clipboard';
  return keepHidden({ clipboardPolicy: { textToRemote: text, textToLocal: text, filesToRemote: all, filesToLocal: all }, devicePolicy: Object.fromEntries(deviceItems.map(([key]) => [key, all])) }, base);
}
export const presetOf = value => presetChoices.map(([key]) => key).find(key => samePermissions(value, presetValue(key, value), { shown: true })) ?? 'custom';
export const sharingName = value => ({ default: t('permissions.17'), clipboard: t('permissions.18'), all: t('permissions.19'), custom: t('common.50') })[presetOf(value)];
// One line that says what is shared: "글·그림 복사 양쪽, 파일은 데스크톱으로만, 프린터".
export function sharingSentence(value) {
  const policy = value.clipboardPolicy, both = (toRemote, toLocal, name, one) => toRemote && toLocal ? t('permissions.20', { name }) : toRemote ? t('permissions.21', { one }) : toLocal ? t('permissions.22', { one }) : null;
  const parts = [both(policy.textToRemote, policy.textToLocal, t('permissions.18'), t('permissions.23')), both(policy.filesToRemote, policy.filesToLocal, t('common.102'), t('permissions.24')),
    ...deviceItems.filter(([key]) => value.devicePolicy[key]).map(([, label]) => label)].filter(Boolean);
  return parts.length ? parts.join(', ') : t('permissions.25');
}

const identityError = code => Object.assign(new Error(messages[code]), { code });
export function accountOwner(user) {
  const id = user?.id, username = user?.username;
  if (id == null || id === '' || typeof username !== 'string' || !username) throw identityError('account_identity_required');
  if (typeof id !== 'string' || !/^[A-Za-z0-9_-]{1,128}$/.test(id)) throw identityError('invalid_account_identity');
  return Object.freeze({ id, username });
}
export function accountCurrent(ctx, value) {
  let owner; try { owner = accountOwner(value); } catch { return false; }
  const users = ctx.state.data?.users;
  if (!Array.isArray(users)) return false;
  const matching = users.filter(user => typeof user.username === 'string' && user.username.toLowerCase() === owner.username.toLowerCase());
  return matching.length === 1 && users.filter(user => user.id === owner.id).length === 1 &&
    matching[0].id === owner.id && matching[0].username === owner.username;
}
export function requireAccountOwner(ctx, value) {
  const owner = accountOwner(value);
  if (!accountCurrent(ctx, owner)) throw identityError('account_identity_changed');
  if (ctx.state.data.users.find(user => user.id === owner.id).disabled) throw identityError('user_or_desktop_unavailable');
  return owner;
}
// A stale/unknown account cannot read another account's saved value.
export const savedPermissions = (ctx, owner, desktopId) => exactPermissions(accountCurrent(ctx, owner)
  ? ctx.state.data.assignments.find(a => a.username === owner.username && a.desktop_id === desktopId) : undefined);
// The registered desktops an account may be given: every registered VM that
// Proxmox still has, and every approved local PC.
export function assignable(ctx) {
  const registered = registeredHostIds(ctx);
  return ctx.state.data.desktops.filter(d => registered?.has(d.id) && (localPc(d) || d.kind !== 'pc' && d.pve_available !== 0));
}

// Whether the account is connected to the desktop now; null while the session list is unknown.
export function connectedTo(ctx, username, desktopId) {
  const sessions = ctx.value('sessions')?.sessions;
  return Array.isArray(sessions) ? sessions.some(session => session.account === username && session.desktopId === desktopId) : null;
}
const turnedOff = (before, after) => ({ clipboard: clipboardKeys.some(key => before.clipboardPolicy[key] && !after.clipboardPolicy[key]),
  devices: deviceItems.filter(([key]) => before.devicePolicy[key] && !after.devicePolicy[key]).map(([, label]) => label) });
const policyOf = value => ({ clipboardPolicy: value.clipboardPolicy, devicePolicy: value.devicePolicy });
// The line beside a row that was just saved (a few seconds).
const savedFor = 6000;
export const savedMark = (ctx, key) => { const mark = ctx.state.ui.sharingSaved; return mark && mark.keys.includes(key) && Date.now() - mark.at < savedFor ? el('span', { class: 'saved-mark', role: 'status' }, icon('check'), t('permissions.26')) : null; };
function markSaved(ctx, keys) {
  const ui = ctx.state.ui, action = ctx.pageAction('sharing-saved');
  ui.sharingSaved = { keys, at: Date.now() };
  const timer = setTimeout(() => { if (action.current() && ctx.state.user && ctx.state.data) ctx.render(); }, savedFor + 100); timer.unref?.();
}
// Saves `entries` ([{ owner, desktop, before, after }]) through `request`. It
// asks first only when something is turned off for someone connected (or the
// session list is unknown): a clipboard item ends that connection, a device
// only leaves it.
function saveSharing(ctx, entries, request, text) {
  if (ctx.state.assignmentSaving || ctx.state.pveBusy) return;
  entries = entries.filter(entry => !samePermissions(entry.before, entry.after));
  if (!entries.length) { ctx.render(); return; }
  const work = async () => {
    const action = ctx.pageAction('assignment-save');
    for (const entry of entries) requireAccountOwner(ctx, entry.owner);
    ctx.state.assignmentSaving = true; ctx.render();
    try {
      await request(entries); action.check();
      await ctx.refresh(); action.check();
      markSaved(ctx, entries.map(entry => `${entry.owner.id}:${entry.desktop.id}`));
      ctx.notice(t('permissions.27') + text(entries));
    } finally { if (action.current()) { ctx.state.assignmentSaving = false; if (ctx.state.user) ctx.render(); } }
  };
  const losing = entries.map(entry => ({ ...entry, off: turnedOff(entry.before, entry.after) })).filter(entry => entry.off.clipboard || entry.off.devices.length);
  const known = !!ctx.value('sessions'), live = losing.filter(entry => connectedTo(ctx, entry.owner.username, entry.desktop.id));
  if (!losing.length || known && !live.length) { void work().catch(error => { if (error.code !== 'admin_session_changed') ctx.notice(error.message, true); }); return; }
  const hit = known ? live : losing, clipboard = hit.some(entry => entry.off.clipboard), devices = [...new Set(hit.flatMap(entry => entry.off.devices))];
  // The switch that was clicked goes back until the answer.
  ctx.render();
  ctx.modal(clipboard && !devices.length ? t('permissions.28') : clipboard ? t('permissions.29') : t('permissions.30', { join: devices.join('·') }), [
    { node: el('p', { class: 'confirm-who' }, icon('user'), el('span', {}, known ? t('permissions.31', { value: [...new Set(live.map(entry => entry.owner.username))].join(', '), value2: [...new Set(live.map(entry => entry.desktop.name))].join(', ') }) : t('common.44'))) },
    { node: el('p', { class: 'modal-description' }, clipboard ? t('permissions.32') : t('permissions.33')) },
  ], work, { save: clipboard ? t('permissions.34') : t('common.45'), danger: true, safeFocus: true, reload: false });
}
// Several accounts of one desktop in one atomic request (PUT /desktops/:id/permissions).
export function saveDesktopSharing(ctx, desktop, changes, { everyone = false } = {}) {
  let entries;
  try { entries = changes.map(({ owner, value }) => { const current = requireAccountOwner(ctx, owner), before = savedPermissions(ctx, current, desktop.id); return { owner: current, desktop, before, after: keepHidden(value, before) }; }); }
  catch (error) { ctx.notice(error.message, true); return; }
  saveSharing(ctx, entries, list => ctx.api('/desktops/' + encodeURIComponent(desktop.id) + '/permissions', 'PUT', { permissions: list.map(entry => ({ username: entry.owner.username, expectedAccountId: entry.owner.id, ...policyOf(entry.after) })) }),
    list => everyone ? t('permissions.35', { name: desktop.name, listCount: list.length, sharingName: sharingName(list[0].after) }) : `${list[0].owner.username} · ${desktop.name}: ${sharingName(list[0].after)}`);
}
// Several desktops of one account in one atomic request (PUT /assignments/batch):
// every assigned desktop is named, and only the changed ones carry a value.
export function saveUserSharing(ctx, owner, changes) {
  let entries, current;
  try { current = requireAccountOwner(ctx, owner); entries = changes.map(({ desktop, value }) => { const before = savedPermissions(ctx, current, desktop.id); return { owner: current, desktop, before, after: keepHidden(value, before) }; }); }
  catch (error) { ctx.notice(error.message, true); return; }
  saveSharing(ctx, entries, list => {
    const changed = new Map(list.map(entry => [entry.desktop.id, entry])), available = new Set(assignable(ctx).map(d => d.id));
    const assignments = ctx.state.data.assignments.filter(a => a.username === current.username && available.has(a.desktop_id)).map(a => changed.has(a.desktop_id) ? { desktopId: a.desktop_id, ...policyOf(changed.get(a.desktop_id).after) } : { desktopId: a.desktop_id });
    return ctx.api('/assignments/batch', 'PUT', { username: current.username, expectedAccountId: current.id, assignments });
  }, list => list.length > 1 ? t('permissions.36', { username: current.username, listCount: list.length, sharingName: sharingName(list[0].after) }) : `${current.username} · ${list[0].desktop.name}: ${sharingName(list[0].after)}`);
}
// 빼기: the account loses this desktop and the assignment's sharing with it.
export function removeAssignment(ctx, owner, desktop) {
  if (ctx.state.assignmentSaving || ctx.state.pveBusy) return;
  let current;
  try { current = requireAccountOwner(ctx, owner); }
  catch (error) { ctx.notice(error.message, true); return; }
  const live = connectedTo(ctx, current.username, desktop.id);
  ctx.modal(t('permissions.37'), [
    { label: t('common.7'), value: current.username, readonly: true }, { label: t('common.5'), value: `${localPc(desktop) ? t('common.22') : `VM ${desktop.pve_vmid}`} · ${desktop.name}`, readonly: true },
    ...(live !== false ? [{ node: el('p', { class: 'confirm-who' }, icon('user'), el('span', {}, live ? t('permissions.38', { username: current.username }) : t('permissions.39'))) }] : []),
  ], async () => {
    const action = ctx.pageAction('assignment-save');
    requireAccountOwner(ctx, current);
    const available = new Set(assignable(ctx).map(d => d.id));
    const desktopIds = ctx.state.data.assignments.filter(a => a.username === current.username && a.desktop_id !== desktop.id && available.has(a.desktop_id)).map(a => a.desktop_id);
    ctx.state.assignmentSaving = true;
    try {
      await ctx.api('/assignments/batch', 'PUT', { username: current.username, expectedAccountId: current.id, desktopIds });
      action.check(); await ctx.refresh(); action.check();
      ctx.notice(t('permissions.40', { username: current.username, name: desktop.name }));
    } finally { if (action.current()) { ctx.state.assignmentSaving = false; if (ctx.state.user) ctx.render(); } }
  }, { save: t('common.52'), danger: true, safeFocus: true, reload: false, description: t('permissions.41') });
}

// The named choices of one assignment as a row of buttons; 직접 설정 is marked
// when the switches match none of them.
export function presetButtons(ctx, { owner, desktop, value, locked = null, save }) {
  const current = presetOf(value), key = `${owner.id}:${desktop.id}`;
  return el('div', { class: 'segmented preset-seg', role: 'group', 'aria-label': t('permissions.42', { username: owner.username, name: desktop.name }) },
    presetChoices.map(([preset, label]) => disable(button(label, () => { if (preset !== current) save(presetValue(preset, value)); }, 'segment' + (current === preset ? ' selected' : ''), null,
      { 'aria-pressed': String(current === preset), 'data-preset': preset, 'data-vm': desktop.id, 'data-account': owner.username, 'data-focus': `preset:${key}:${preset}` }), locked)),
    current === 'custom' ? el('span', { class: 'segment selected is-custom', 'data-preset': 'custom' }, t('common.50')) : null);
}
// Every shown item as a switch; each change is saved at once.
export function sharingSwitches(ctx, { owner, desktop, value, locked = null, save }) {
  const key = `${owner.id}:${desktop.id}`;
  const toggle = (group, item, label, text = null) => el('label', { class: 'switch-label', title: locked }, text,
    el('input', { type: 'checkbox', role: 'switch', class: 'switch-input', checked: value[group][item], disabled: !!locked, 'aria-label': label,
      [group === 'clipboardPolicy' ? 'data-clipboard-permission' : 'data-device-permission']: item, 'data-vm': desktop.id, 'data-account': owner.username, 'data-focus': `perm:${key}:${item}`,
      onchange: event => { const next = structuredClone(value); next[group][item] = event.target.checked === true; save(next); } }),
    el('span', { class: 'switch-track', 'aria-hidden': 'true' }));
  const line = (title, text, controls, note = null) => el('div', { class: 'row-line' }, el('div', { class: 'row-text', title: note }, el('strong', {}, title), text ? el('small', {}, text) : null), el('div', { class: 'row-actions' }, controls));
  const device = ([item, label, text, note]) => line(label, text, toggle('devicePolicy', item, t('permissions.43', { label })), note);
  return el('div', { class: 'sharing-fine', id: 'sharing-' + key },
    el('h4', {}, t('permissions.44')),
    line(t('permissions.45'), t('permissions.46'), clipboardDirections.map(([suffix, direction]) => toggle('clipboardPolicy', 'text' + suffix, t('permissions.47', { direction }), direction))),
    line(t('common.102'), t('permissions.48'), clipboardDirections.map(([suffix, direction]) => toggle('clipboardPolicy', 'files' + suffix, t('permissions.49', { direction }), direction))),
    el('h4', {}, t('common.112')),
    deviceItems.filter(([item]) => !rareDeviceKeys.has(item)).map(device),
    el('details', { class: 'sharing-rare', 'data-keep-open': 'sharing-rare:' + key, open: deviceItems.some(([item]) => rareDeviceKeys.has(item) && value.devicePolicy[item]) }, el('summary', {}, t('permissions.50')), deviceItems.filter(([item]) => rareDeviceKeys.has(item)).map(device)),
    el('details', { class: 'sharing-help', 'data-keep-open': 'sharing-help:' + key }, el('summary', {}, t('common.80')), el('p', {}, applyHint)));
}
// Both together, where a row opens its sharing in place (the desktop's 쓰는 사람).
export const sharingControl = (ctx, options) => el('div', { class: 'sharing-control' }, el('div', { class: 'sharing-control-head' }, presetButtons(ctx, options), savedMark(ctx, `${options.owner.id}:${options.desktop.id}`)), sharingSwitches(ctx, options));

export const applyHint = t('permissions.51');
