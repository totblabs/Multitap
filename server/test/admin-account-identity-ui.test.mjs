import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { messages } from '../admin/js/labels.js';

// Execute the actual shipped forms. Add test exports only to an in-memory
// module copy; modal/navigation/API collaborators are inert and no DOM,
// listener, credentials or production data are used.
const url = new URL('../admin/js/users.js', import.meta.url);
const source = (await readFile(url, 'utf8')).replace(/from\s+(['"])(\.[^'"]+)\1/g,
  (_, quote, path) => `from ${quote}${new URL(path, url).href}${quote}`);
const forms = await import('data:text/javascript;base64,' + Buffer.from(source +
  '\nexport { passwordForm, enableForm, deleteForm };').toString('base64'));
const account = () => ({ id: 'account_A-1', username: 'alice', role: 'user', notes: '', disabled: false });
const fixture = () => {
  const requests = [], notices = [], modals = [];
  const ctx = {
    state: { ui: {}, data: { users: [{ id: 'created_1', username: 'created' }], assignments: [] } },
    userByName: () => null,
    // These forms read the connections (ctx.value('sessions')) to say when the person is connected now; none is known here.
    value: () => null,
    modal(title, fields, run, options) { modals.push({ title, fields, run, options }); },
    confirmAction(title, description, run) { modals.push({ title, description, run }); },
    async change(path, method, data) { requests.push({ path, method, data }); return { ok: true }; },
    async api(path, method, data) { requests.push({ path, method, data }); return { ok: true }; },
    pageAction: () => ({ check() {}, current: () => true }),
    async refresh() {}, render() {}, navigate() {},
    notice: (...args) => notices.push(args),
  };
  return { ctx, requests, notices, modals };
};
const actions = [
  ['profile', 'userForm', '', 'PUT', { role: 'admin', notes: 'fixture note' }],
  ['password', 'passwordForm', '/password', 'PUT', { password: 'fixture-password', passwordConfirmation: 'fixture-password' }],
  ['stop', 'enableForm', '/enabled', 'PUT', { enabled: false }],
  ['delete', 'deleteForm', '', 'DELETE', {}],
];
for (const [label, name, suffix, method, input] of actions) {
  test(`${label} keeps the immutable ID and username shown when the action opened`, async () => {
    const f = fixture(), user = account();
    await forms[name](f.ctx, user);
    assert.equal(f.modals.length, 1);
    user.id = 'account_B-2'; user.username = 'bob';
    await f.modals[0].run(input);
    assert.equal(f.requests.length, 1);
    assert.equal(f.requests[0].path, '/users/alice' + suffix);
    assert.equal(f.requests[0].method, method);
    assert.deepEqual(f.requests[0].data, { ...input, expectedAccountId: 'account_A-1' });
  });
  test(`${label} refuses a legacy account without immutable identity before opening`, async () => {
    const f = fixture(), user = account(); delete user.id;
    await forms[name](f.ctx, user);
    assert.equal(f.requests.length, 0);
    assert.equal(f.modals.length, 0);
    assert.equal(f.notices.length, 1);
    assert.equal(f.notices[0][0], messages.account_identity_required);
    assert.equal(f.notices[0][1], true);
  });
}
test('immediate enable captures identity before the pending request yields', async () => {
  const f = fixture(), user = { ...account(), disabled: true };
  let finish;
  f.ctx.change = (path, method, data) => {
    f.requests.push({ path, method, data });
    return new Promise(resolve => { finish = resolve; });
  };
  const operation = forms.enableForm(f.ctx, user);
  user.id = 'account_B-2'; user.username = 'bob';
  assert.deepEqual(f.requests, [{ path: '/users/alice/enabled', method: 'PUT',
    data: { enabled: true, expectedAccountId: 'account_A-1' } }]);
  finish({ ok: true }); await operation;
});
test('untrusted form values cannot override the ID captured for the profile', async () => {
  const f = fixture(); forms.userForm(f.ctx, account());
  await f.modals[0].run({ role: 'user', expectedAccountId: 'account_B-2' });
  assert.equal(f.requests[0].data.expectedAccountId, 'account_A-1');
});
test('malformed immutable account IDs refuse before a password dialog or request', () => {
  for (const id of ['bad id', 'x'.repeat(129), '\uacc4\uc815', { id: 'nested' }]) {
    const f = fixture(); forms.passwordForm(f.ctx, { ...account(), id });
    assert.equal(f.requests.length, 0); assert.equal(f.modals.length, 0);
    assert.equal(f.notices[0]?.[0], messages.invalid_account_identity);
  }
});
test('account creation preserves the existing payload without an identity precondition', async () => {
  const f = fixture(), input = { username: 'created', role: 'user', password: 'fixture-password', passwordConfirmation: 'fixture-password' };
  forms.userForm(f.ctx);
  await f.modals[0].run(input);
  assert.deepEqual(f.requests, [{ path: '/users', method: 'POST', data: input }]);
});
test('a server identity conflict is returned to the modal without a mutation retry', async () => {
  const f = fixture(), conflict = Object.assign(new Error('Fixture identity changed'), { code: 'account_identity_changed' });
  f.ctx.change = async (path, method, data) => { f.requests.push({ path, method, data }); throw conflict; };
  forms.passwordForm(f.ctx, account());
  await assert.rejects(f.modals[0].run({ password: 'fixture-password', passwordConfirmation: 'fixture-password' }), error => error === conflict);
  assert.equal(f.requests.length, 1);
  assert.equal(f.requests[0].data.expectedAccountId, 'account_A-1');
});
test('all three fixed identity errors give a refresh action', () => {
  for (const code of ['account_identity_required', 'invalid_account_identity', 'account_identity_changed']) {
    assert.equal(typeof messages[code], 'string', code);
    assert.ok(messages[code].includes('\uc0c8\ub85c\uace0\uce68'), code);
  }
});
