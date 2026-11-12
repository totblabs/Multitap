// 사용자: the accounts as a list, and one account in its window (창). Its
// desktops are listed there with their sharing as one named choice each;
// assigning, removing and every sharing change is saved at once
// (permissions.js).
import { el, icon, button, stateDot, panel, facts, emptyState, loadingState, errorState, select, disable, tally, listHead, queryBox, queryField, queryText, board, detailWindow } from './dom.js';
import { vmOrder, desktopKind, desktopLabel } from './desktops.js';
import { vmSearchText, messages } from './labels.js';
import { accountOwner, accountCurrent, requireAccountOwner, savedPermissions, sharingSentence, presetButtons, sharingSwitches, savedMark, saveUserSharing, removeAssignment, presetChoices, presetValue, assignable } from './permissions.js';
import { collect, logBoard } from './timeline.js';
import { localPc, registeredHostIds } from './hosts.js';
import { openLog } from './monitor.js';
import { openNotices } from './notices.js';
import { t } from './i18n.js';

const assignmentOrder = (a, b) => Number(localPc(a)) - Number(localPc(b)) || (localPc(a) ? a.name.localeCompare(b.name, 'ko') : vmOrder(a, b));

const userPath = user => '/users/' + encodeURIComponent(user.username);
// Keep the identity actually shown at open time. A mutable user object or a
// later username lookup may already refer to a different account at submit.
const mutationTarget = (ctx, user) => {
  const expectedAccountId = user?.id, username = user?.username;
  const code = expectedAccountId == null || expectedAccountId === '' || typeof username !== 'string' || !username
    ? 'account_identity_required'
    : typeof expectedAccountId !== 'string' || !/^[A-Za-z0-9_-]{1,128}$/.test(expectedAccountId)
      ? 'invalid_account_identity' : null;
  if (code) { ctx.notice(messages[code], true); return null; }
  return { expectedAccountId, username, path: userPath({ username }) };
};
const passwordFields = label => [{ name: 'password', label, type: 'password', min: 10, max: 256 }, { name: 'passwordConfirmation', label: t('users.1'), type: 'password', min: 10, max: 256 }];
const roleText = user => user.is_primary_admin ? t('users.2') : user.role === 'admin' ? t('common.24') : t('users.roleUser');
export const findUser = (ctx, name) => ctx.userByName(name) ?? ctx.state.data.users.find(u => u.username.toLowerCase() === String(name).toLowerCase()) ?? null;

export function userForm(ctx, user = null) {
  const target = user ? mutationTarget(ctx, user) : null;
  if (user && !target) return;
  const fields = [
    user ? { label: t('users.3'), value: user.username, readonly: true } : { name: 'username', label: t('users.3'), min: 2, max: 64, hint: t('users.4') },
    user?.is_primary_admin ? { label: t('users.5'), value: t('users.2'), readonly: true, hint: t('users.6') } : { name: 'role', label: t('users.5'), value: user?.role || 'user', options: [['user', t('users.7')], ['admin', t('common.24')]] },
    { name: 'notes', label: t('users.8'), value: user?.notes, required: false, multiline: true, max: 500, placeholder: t('users.9') },
    ...(!user ? passwordFields(t('users.10')) : []),
  ];
  ctx.modal(user ? t('users.11') : t('common.139'), fields, async input => {
    const action = ctx.pageAction('user-form');
    if (user) { await ctx.change(target.path, 'PUT', { ...input, expectedAccountId: target.expectedAccountId }); action.check(); await ctx.refresh(); action.check(); return; }
    await ctx.api('/users', 'POST', input); action.check(); await ctx.refresh(); action.check();
    // The new account's window opens next, on 데스크톱, so its desktops can be assigned right away.
    const made = findUser(ctx, input.username);
    if (made) ctx.state.ui.detailTab = { key: 'user:' + made.id, tab: 'desktops' };
    ctx.navigate('user', made?.username ?? input.username);
    ctx.notice(t('users.12', { username: input.username }));
  }, { reload: false, save: user ? t('common.20') : t('common.139'), description: user?.is_primary_admin ? t('users.13') : user ? t('users.14') : t('users.15') });
}
// 계정 중지, 계정 삭제 and 비밀번호 재설정 end the account's sign-ins, and a connection goes with its sign-in: the
// confirmation says so when the person is connected now, as the other confirmations that end a connection do.
const liveLine = (ctx, user) => liveSessions(ctx, user).length ? [{ node: el('p', { class: 'confirm-who' }, icon('user'), el('span', {}, t('permissions.38', { username: user.username }))) }] : [];
const passwordForm = (ctx, user) => {
  if (user.is_primary_admin && user.id !== ctx.state.user?.id) return;
  const target = mutationTarget(ctx, user);
  if (!target) return;
  return ctx.modal(t('users.16'), [...liveLine(ctx, user), ...passwordFields(t('users.17'))], input => ctx.change(target.path + '/password', 'PUT', { ...input, expectedAccountId: target.expectedAccountId }, t('users.18', { username: target.username })), { save: t('users.16'), description: t('users.19', { username: target.username }) });
};
// Stopping an account ends its sign-ins, so it asks once; turning it back on
// takes nothing away and happens at once.
async function enableForm(ctx, user) {
  const target = mutationTarget(ctx, user);
  if (!target) return;
  if (!user.disabled) { ctx.modal(t('users.20'), liveLine(ctx, user), () => ctx.change(target.path + '/enabled', 'PUT', { enabled: false, expectedAccountId: target.expectedAccountId }, t('users.22', { username: target.username })), { description: t('users.21', { username: target.username }), save: t('users.20'), danger: true }); return; }
  if (ctx.state.ui.accountEnabling) return;
  const ui = ctx.state.ui, action = ctx.pageAction('account-enable');
  ui.accountEnabling = true; ctx.render();
  try { await ctx.change(target.path + '/enabled', 'PUT', { enabled: true, expectedAccountId: target.expectedAccountId }, t('users.23', { username: target.username })); action.check(); await ctx.refresh(); action.check(); }
  catch (error) { if (action.current()) ctx.notice(error.message, true); }
  finally { if (action.current()) { ui.accountEnabling = false; if (ctx.state.user) ctx.render(); } }
}
const deleteForm = (ctx, user) => {
  const target = mutationTarget(ctx, user);
  if (!target) return;
  const count = assignmentCount(ctx, user);
  ctx.modal(t('users.24'), liveLine(ctx, user), async () => { await ctx.api(target.path, 'DELETE', { expectedAccountId: target.expectedAccountId }); ctx.navigate('users'); ctx.notice(t('users.27', { username: target.username })); }, { description: t('users.26', { username: target.username, value: count ? t('users.25', { count }) : '' }), save: t('users.24'), danger: true });
};
const assignmentCount = (ctx, user) => ctx.state.data.assignments.filter(a => a.username === user.username).length;
const liveSessions = (ctx, user) => (ctx.value('sessions')?.sessions ?? []).filter(s => s.account === user.username);

// What the list shows (role, state, search text), and the conditions as they stand in the 조회 조건 box: what
// was changed there and not applied with 조회 yet.
const listUi = ctx => ctx.state.ui.users ??= { query: '', role: 'all', status: 'all' };
function listDraft(ctx) {
  const ui = listUi(ctx), of = [ui.role, ui.status, ui.query].join('\n');
  return ui.draft?.of === of ? ui.draft : { role: ui.role, status: ui.status, query: ui.query, of };
}
const columns = [{ label: t('common.84'), cls: 'col-num' }, { label: t('users.3'), cls: 'col-id is-name' }, { label: t('users.28'), cls: 'col-memo is-left' }, { label: t('common.5'), cls: 'col-count' }, { label: t('common.61'), cls: 'col-state is-state' }, { label: t('common.85'), cls: 'col-manage' }];
// The accounts as a board: administrators first, under their own heading, then everyone else; a group without
// accounts (a filter, a search) shows no heading. `current` is the account whose window is open.
function accountList(ctx, current = null) {
  const ui = listUi(ctx), draft = listDraft(ctx), query = ui.query.toLowerCase();
  const all = ctx.state.data.users, users = all.filter(u => `${u.username} ${u.notes ?? ''}`.toLowerCase().includes(query) &&
    (ui.role === 'all' || u.role === ui.role) && (ui.status === 'all' || (ui.status === 'disabled') === !!u.disabled));
  const known = !!ctx.value('sessions'), stopped = all.filter(u => u.disabled).length;
  const change = values => { ui.draft = { ...listDraft(ctx), ...values }; };
  const apply = (refocus = { key: 'query-apply' }) => { const next = listDraft(ctx); ui.role = next.role; ui.status = next.status; ui.query = next.query; ui.draft = null; ctx.render({ refocus }); };
  const clear = () => { ui.role = 'all'; ui.status = 'all'; ui.query = ''; ui.draft = null; ctx.render(); };
  const conditions = queryBox(t('users.29'), [
    queryField(t('users.5'), select(t('users.30'), [['all', t('users.31')], ['admin', t('common.24')], ['user', t('users.7')]], draft.role, value => change({ role: value }), { attrs: { 'data-focus': 'users-role' } })),
    queryField(t('common.61'), select(t('users.32'), [['all', t('users.33')], ['active', t('users.34')], ['disabled', t('common.25')]], draft.status, value => change({ status: value }), { attrs: { 'data-focus': 'users-status' } })),
    queryField(t('common.89'), queryText({ key: 'users-search', value: draft.query, placeholder: t('users.35'), onInput: value => change({ query: value }), onApply: apply }), 'is-search')], apply, clear);
  let shown = 0;
  const item = u => {
    const live = liveSessions(ctx, u).length > 0, count = assignmentCount(ctx, u), memo = u.notes ? u.notes.split('\n')[0] : t('users.36');
    const open = () => ctx.navigate('user', u.username);
    return { attrs: { class: 'is-pick' + (current?.id === u.id ? ' is-on' : ''), onclick: event => { if (!event.target?.closest?.('a,button')) open(); } }, cells: [
      String(++shown),
      // The role stands beside the name as plain text; 메모 is what was written as one, here and in the window.
      [ctx.link(el('span', { class: 'account-choice-text' }, el('strong', {}, u.username), el('small', {}, t('users.37', { memo, count }))), 'user', u.username,
        { class: 'account-choice', 'aria-label': u.username, 'aria-description': t('users.39', { roleText: roleText(u), count, value: u.disabled ? t('common.25') : t('users.34'), value2: live ? t('users.38') : '' }), 'aria-current': current?.id === u.id ? 'true' : null, 'data-focus': 'account:' + u.id }),
        u.role === 'admin' ? el('small', { class: 'account-role' }, roleText(u)) : null],
      u.notes ? memo : el('span', { class: 'muted' }, memo), t('users.40', { count }), live ? stateDot(t('common.49'), 'green') : u.disabled ? stateDot(t('users.41'), 'muted') : stateDot(t('users.34'), 'blue'),
      button(t('common.91'), open, 'secondary small', null, { 'aria-label': t('users.42', { username: u.username }) })] };
  };
  const part = (label, list) => list.length ? [{ group: [el('span', {}, label), el('span', { class: 'count' }, String(list.length))], cls: 'account-group' }, ...list.map(item)] : [];
  const rows = [...part(t('common.24'), users.filter(u => u.role === 'admin')), ...part(t('common.7'), users.filter(u => u.role !== 'admin'))];
  const head = listHead(tally([[t('common.92'), all.length, t('common.105')], [t('common.24'), all.filter(u => u.role === 'admin').length], known ? [t('common.49'), all.filter(u => liveSessions(ctx, u).length).length] : null, stopped ? [t('users.41'), stopped] : null],
      users.length !== all.length ? el('small', { class: 'account-list-caption' }, t('common.97'), el('span', {}, t('users.43', { usersCount: users.length, allCount: all.length }))) : null),
    [button(t('common.139'), () => userForm(ctx), 'primary', 'plus')]);
  return [conditions, el('section', { class: 'account-list', 'aria-label': t('users.44') }, head,
    board(columns, rows, { className: 'account-table', empty: emptyState(t('users.45'), t('users.46'), { compact: true }) }))];
}
const pageHead = ctx => ctx.pageHeader({ title: t('common.7') });

export function list(ctx) { return [pageHead(ctx), accountList(ctx)]; }

// 데스크톱 배정: the registered desktops this account does not have yet. The
// choice is saved at once and each new one starts with nothing shared.
function assignDesktops(ctx, user) {
  if (ctx.state.assignmentSaving || ctx.state.pveBusy) return;
  let owner;
  try { owner = requireAccountOwner(ctx, user); } catch (error) { ctx.notice(error.message, true); return; }
  if (!registeredHostIds(ctx)) { ctx.notice(messages.host_identity_required, true); return; }
  const has = new Set(ctx.state.data.assignments.filter(a => a.username === owner.username).map(a => a.desktop_id));
  const choices = assignable(ctx).filter(d => !has.has(d.id)).sort(assignmentOrder);
  if (!choices.length) { ctx.notice(t('users.48'), true); return; }
  const boxes = new Map(), items = choices.map(d => {
    const box = el('input', { type: 'checkbox', 'data-vm-id': d.id, 'aria-label': t('users.49', { desktopLabel: desktopLabel(d) }) }); boxes.set(d.id, box);
    return el('label', { class: 'pick-item', 'data-search': `${localPc(d) ? t('users.50') : 'vm '}${vmSearchText(d)}` }, box, el('span', {}, el('strong', {}, d.name), el('small', {}, localPc(d) ? t('common.22') : `VM ${d.pve_vmid} · ${d.pve_node || t('common.77')}`)));
  });
  const filter = el('input', { type: 'search', placeholder: t('users.51'), 'aria-label': t('users.52'), oninput: event => { const q = event.target.value.trim().toLowerCase(); for (const node of items) node.hidden = !!q && !node.getAttribute('data-search').includes(q) && !node.querySelector('input').checked; } });
  ctx.modal(t('common.140'), [{ label: t('common.7'), value: owner.username, readonly: true }, { node: el('div', { class: 'pick-list-wrap' }, choices.length > 6 ? filter : null, el('div', { class: 'pick-list', role: 'group', 'aria-label': t('users.53') }, items)) }], async () => {
    const picked = choices.filter(d => boxes.get(d.id).checked === true).map(d => d.id);
    if (!picked.length) throw new Error(t('users.54'));
    const current = requireAccountOwner(ctx, owner), action = ctx.pageAction('assignment-save');
    const registered = registeredHostIds(ctx);
    if (!registered || picked.some(id => !registered.has(id))) throw new Error(messages.host_not_registered);
    const available = new Set(assignable(ctx).map(d => d.id));
    const saved = ctx.state.data.assignments.filter(a => a.username === current.username && available.has(a.desktop_id)).map(a => a.desktop_id);
    ctx.state.assignmentSaving = true;
    try {
      const result = await ctx.api('/assignments/batch', 'PUT', { username: current.username, expectedAccountId: current.id, desktopIds: [...new Set([...saved, ...picked])] });
      action.check(); await ctx.refresh(); action.check();
      ctx.notice(t('users.55', { username: current.username, value: result.added ?? picked.length }));
    } finally { if (action.current()) { ctx.state.assignmentSaving = false; if (ctx.state.user) ctx.render(); } }
  }, { save: t('common.35'), reload: false, description: t('common.48') });
}

// 쓸 수 있는 데스크톱: one row per assignment with its sharing choice.
function desktopsPanel(ctx, user) {
  const owner = accountOwner(user);
  if (!registeredHostIds(ctx)) {
    const entry = ctx.resource('hosts');
    return panel({ title: t('users.56'), id: 'assignments' }, entry?.error || entry?.value
      ? errorState(t('common.82'), entry.error || messages.host_identity_required, () => ctx.load('hosts', { force: true }))
      : loadingState(t('common.83')));
  }
  const available = new Map(assignable(ctx).map(d => [d.id, d]));
  const assigned = ctx.state.data.assignments.filter(a => a.username === owner.username && available.has(a.desktop_id)).map(a => available.get(a.desktop_id)).sort(assignmentOrder);
  const current = accountCurrent(ctx, owner);
  const locked = ctx.state.assignmentSaving ? t('users.57') : ctx.state.pveBusy ? t('common.149') : user.disabled ? t('common.51') : !current ? messages.account_identity_changed : null;
  const ui = ctx.state.ui, live = new Set(liveSessions(ctx, user).map(s => s.desktopId));
  const rows = assigned.map(d => {
    const key = `${owner.id}:${d.id}`, open = ui.sharingOpen === key, value = savedPermissions(ctx, owner, d.id);
    const options = { owner, desktop: d, value, locked, save: next => saveUserSharing(ctx, owner, [{ desktop: d, value: next }]) };
    const toggle = () => { ui.sharingOpen = open ? null : key; ctx.render({ refocus: { key: 'share-toggle:' + key } }); };
    return el('li', { class: 'assigned-row' + (open ? ' is-open' : ''), 'data-desktop': d.id },
      el('div', { class: 'row-line' },
        el('div', { class: 'row-text' }, el('strong', {}, ctx.link(d.name, 'desktop', d.id, { class: 'row-link' }), live.has(d.id) ? stateDot(t('common.49'), 'green') : null, savedMark(ctx, key)), el('small', {}, `${desktopKind(d)} · ${sharingSentence(value)}`)),
        el('div', { class: 'row-actions' }, presetButtons(ctx, options),
          button(open ? t('common.31') : t('users.58'), toggle, 'quiet small', null, { 'aria-expanded': String(open), 'aria-controls': 'sharing-' + key, 'aria-label': t('users.59', { name: d.name, value: open ? t('common.31') : t('users.58') }), 'data-permission-vm': d.id, 'data-focus': 'share-toggle:' + key }),
          disable(button(t('common.52'), () => removeAssignment(ctx, owner, d), 'quiet small', null, { 'aria-label': t('common.53', { name: d.name }), 'data-remove-vm': d.id }), locked))),
      open ? sharingSwitches(ctx, options) : null);
  });
  // The same choice for every desktop of this account, in one atomic request.
  const everything = assigned.length > 1 ? el('div', { class: 'same-for-all', role: 'group', 'aria-label': t('users.60') }, el('span', {}, t('users.61')),
    presetChoices.map(([preset, label]) => disable(button(label, () => saveUserSharing(ctx, owner, assigned.map(d => ({ desktop: d, value: presetValue(preset, savedPermissions(ctx, owner, d.id)) }))), 'quiet small', null, { 'data-same-preset': preset }), locked))) : null;
  return panel({ title: t('users.56'), count: assigned.length, id: 'assignments', className: 'assigned-panel',
    actions: [disable(button(t('common.140'), () => assignDesktops(ctx, user), assigned.length ? 'secondary' : 'primary', 'plus'), locked)] },
    assigned.length ? [el('ul', { class: 'row-list' }, rows), everything] : el('p', { class: 'panel-empty' }, user.disabled ? t('users.62') : t('users.63')),
    user.unavailableAssignmentCount ? el('p', { class: 'assignment-retained' }, t('users.64', { unavailableAssignmentCount: user.unavailableAssignmentCount })) : null,
    el('p', { class: 'note' }, icon('info'), el('span', {}, user.disabled ? t('users.65') : t('users.66'))));
}

// The window of one account (dom.js detailWindow): the account in 기본 정보, its desktops and their sharing in
// 데스크톱, its connections and records in 최근 기록, and what can be done to the account in the bar at the bottom.
function accountWindow(ctx, user) {
  const self = user.id === ctx.state.user?.id, live = liveSessions(ctx, user), sessionsEntry = ctx.resource('sessions');
  const liveNames = [...new Set(live.map(s => ctx.desktopById(s.desktopId)?.name ?? s.desktopId))];
  const entries = collect(ctx).filter(e => !e.stats && (e.actorId === user.id || e.userId === user.id || (e.source === 'client' && e.account === user.username)));
  const clientEntry = ctx.resource('clientDiagnostics'), connections = entries.filter(e => e.source === 'client');
  // The account's state as the list shows it: 접속 중 (green), 사용 중 (blue) or 사용 중지 (grey).
  const state = user.disabled ? stateDot(t('common.25'), 'muted') : live.length ? stateDot(t('common.49'), 'green') : stateDot(t('users.34'), 'blue');
  const info = facts([
    [t('users.3'), user.username], [t('users.5'), user.is_primary_admin ? el('span', { title: t('users.67') }, roleText(user)) : roleText(user)],
    [t('common.61'), state.cloneNode(true)], [t('users.assignedLabel'), t('users.68', { assignmentCount: assignmentCount(ctx, user) })],
    [t('users.28'), el('span', { class: 'account-notes' + (user.notes ? '' : ' muted') }, user.notes || t('users.36'))],
    [t('common.69'), !sessionsEntry?.value ? t('common.30') : live.length ? el('ul', { class: 'fact-list' }, live.map(s => { const d = ctx.desktopById(s.desktopId); return el('li', {}, d ? ctx.link(desktopLabel(d), 'desktop', d.id) : el('span', {}, s.desktopId)); })) : t('common.36')],
  ], 'form-table is-pairs');
  const recent = panel({ title: t('users.69'), actions: [button(t('common.73'), () => openLog(ctx, { account: user.username, failures: false }), 'quiet small')] },
    !clientEntry?.value && !clientEntry?.error ? loadingState() : connections.length ? logBoard(ctx, connections, { limit: 4, showAccount: false, inline: 'user-connections' }) : el('p', { class: 'panel-empty' }, t('users.70')));
  const activity = panel({ title: t('users.71'), level: 'h3', description: t('users.72') },
    !clientEntry?.value && !clientEntry?.error ? loadingState() : logBoard(ctx, entries, { limit: 30, showAccount: false, cause: false, inline: 'user-records', emptyTitle: t('users.73') }));
  return detailWindow(ctx, { key: 'user:' + user.id, kind: t('users.74'), className: 'user-detail', headClass: 'account-head', focusKey: 'account:' + user.id, close: () => ctx.navigate('users'),
    // The name alone; what it is (관리자, the account signed in here) and its memo stand after it as plain text.
    title: user.username,
    sub: [user.role === 'admin' ? roleText(user) : null, self ? t('users.75') : null, user.notes ? user.notes.split('\n')[0] : null].filter(Boolean).join(' · '), subTitle: [liveNames.length ? t('users.76', { join: liveNames.join(', ') }) : null, user.notes || null].filter(Boolean).join('\n') || null,
    // While Proxmox is read the sharing and assignment buttons are off: the window says why, as a desktop's does.
    badges: [state, ctx.state.pveBusy ? el('small', { class: 'win-busy', role: 'status' }, t('desktops.105')) : null],
    tabs: [{ key: 'info', label: t('common.78'), nodes: info }, { key: 'desktops', label: t('common.5'), count: assignmentCount(ctx, user), nodes: desktopsPanel(ctx, user) }, { key: 'records', label: t('common.79'), nodes: [recent, activity] }],
    // Plain buttons, 삭제 first (as in the desktop window).
    actions: [!user.is_primary_admin ? button(t('common.147'), () => deleteForm(ctx, user), 'danger dt-delete') : null,
      // Another administrator cannot change the primary administrator's password (server: primary_admin_protected).
      disable(button(t('users.16'), () => passwordForm(ctx, user), 'secondary'), user.is_primary_admin && !self ? t('users.primaryPassword') : null), button(t('users.77'), () => userForm(ctx, user), 'secondary'),
      // 공지 보내기 (js/notices.js): to this person, now or at the next sign-in.
      ...(user.disabled ? [] : [button(t('common.59'), () => openNotices(ctx, { target: 'user', user: user.username }), 'secondary')]),
      ...(!user.is_primary_admin ? [disable(button(user.disabled ? (ctx.state.ui.accountEnabling ? t('users.enablingF3') : t('users.78')) : t('users.20'), () => enableForm(ctx, user), 'secondary'), ctx.state.ui.accountEnabling ? t('users.79') : null)] : [])].filter(Boolean) });
}

export function detail(ctx, route) {
  const user = findUser(ctx, route.id);
  if (!user) return [pageHead(ctx), emptyState(t('users.80'), t('users.81'), { panel: true, iconName: 'user', action: ctx.link(t('users.44'), 'users', null, { class: 'button secondary' }) })];
  return [pageHead(ctx), accountList(ctx, user), accountWindow(ctx, user)];
}
