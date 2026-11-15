// 데스크톱 그룹: each account's own named sections of its desktop list, kept on
// the server (desktop_groups, a desktop's group on its assignment row).
// Real loopback HTTP/WebSocket routing with synthetic Proxmox/Host identities.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';
import { mkdtemp, rm } from 'node:fs/promises';
import { copyFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { groupName, groupEntries, maxGroups } from '../src/desktop-organize.mjs';
import { snapshotDatabase, sanitizeSnapshot, rebuildFromBackup } from '../src/backup-database.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';
import { seedServer, temporaryDirectory, travellingRows, ids, fixtureGroupId } from './backup-fixture.mjs';

const origin = 'https://groups-fixture.example';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const machineUuid = vmid => '12345678-1234-4234-8234-' + String(vmid).padStart(12, '0');
const vm = vmid => ({ vmid, node: 'fixture-node', name: `VM${vmid}`, status: 'running', machineUuid: machineUuid(vmid) });
const groupId = /^g_[0-9a-f]{12}$/;
const throwsCode = (action, code) => assert.throws(action, error => error.message === code, code);

test('Group names are trimmed, 1-40 characters, without control characters; lists are checked whole', () => {
  assert.equal(groupName('  업무용  '), '업무용');
  assert.equal(groupName('a'.repeat(40)), 'a'.repeat(40));
  assert.equal(groupName('가'.repeat(40)), '가'.repeat(40));
  assert.equal(groupName('😀'.repeat(40)), '😀'.repeat(40), 'characters, not UTF-16 units');
  assert.equal(groupName('Lab 1 · 테스트'), 'Lab 1 · 테스트');
  assert.equal(groupName('\u1100\u1161'), '\uac00', 'stored composed (NFC)');
  for (const value of ['', '   ', '\u3000', 'a'.repeat(41), '가'.repeat(41), 'a\nb', 'a\tb', 'a\u0000b', 'a\u007fb', 'a\u0085b', 'a\u2028b', 'a\ud800b', 5, null, undefined, ['x'], {}]) throwsCode(() => groupName(value), 'invalid_group_name');
  assert.deepEqual(groupEntries([]), []);
  assert.deepEqual(groupEntries([{ name: ' A ' }, { id: 'g_000000000001', name: 'B' }, { id: null, name: 'C' }]), [{ id: null, name: 'A' }, { id: 'g_000000000001', name: 'B' }, { id: null, name: 'C' }]);
  assert.equal(groupEntries(Array.from({ length: maxGroups }, (_, index) => ({ name: 'g' + index }))).length, 30);
  throwsCode(() => groupEntries(Array.from({ length: 31 }, (_, index) => ({ name: 'g' + index }))), 'too_many_groups');
  throwsCode(() => groupEntries([{ name: 'Office' }, { name: ' office ' }]), 'duplicate_group_name');
  throwsCode(() => groupEntries([{ name: 'ÉCOLE' }, { name: 'école' }]), 'duplicate_group_name');
  throwsCode(() => groupEntries([{ id: 'g_000000000001', name: 'A' }, { id: 'g_000000000001', name: 'B' }]), 'invalid_groups');
  for (const value of [null, {}, 'x', [null], [[]], ['A'], [{ name: 'A', extra: 1 }], [{ id: 5, name: 'A' }], [{ id: {}, name: 'A' }]]) throwsCode(() => groupEntries(value), 'invalid_groups');
  throwsCode(() => groupEntries([{ name: 'A' }, { name: '' }]), 'invalid_group_name');
  throwsCode(() => groupEntries([{ id: 'g_000000000001' }]), 'invalid_group_name');
});

// A database of the release before groups: the current schema without the
// desktop_groups table and the assignments.group_id column.
function previousRelease(path) {
  const db = new DatabaseSync(path);
  try { db.exec('PRAGMA foreign_keys=OFF; DROP INDEX assignments_group; ALTER TABLE assignments DROP COLUMN group_id; DROP TABLE desktop_groups;'); } finally { db.close(); }
}

test('An existing database gains groups on start: every desktop ungrouped, nothing else changes, and groups survive the next start', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-groups-migration-'));
  t.after(() => rm(directory, { recursive: true }));
  for (const [label, Make] of [['previous release', Store], ['schema 18', Store18]]) {
    const path = join(directory, label.replace(' ', '-') + '.sqlite');
    let store = new Make(path);
    const owner = store.addUser({ name: 'fixture-user', passwordHash: 'retained-password-fixture' });
    store.syncProxmox([vm(710), vm(711)], 'fixture');
    enrollFixtureHosts(store, ['pve-710', 'pve-711']);
    store.assign('fixture-user', 'pve-710', 'fixture'); store.assign('fixture-user', 'pve-711', 'fixture');
    store.updateAssignmentPolicy('fixture-user', 'pve-710', { clipboardPolicy: { textToRemote: true, textToLocal: false, filesToRemote: false, filesToLocal: false } });
    assert.equal(store.setFavorite(owner, 'pve-711', true), true);
    store.close();
    if (Make === Store) previousRelease(path);
    const before = (() => { const db = new DatabaseSync(path, { readOnly: true }); try { return db.prepare('SELECT rowid,* FROM assignments ORDER BY rowid').all().map(row => ({ ...row })); } finally { db.close(); } })();
    assert.equal(before.some(row => 'group_id' in row), false, label + ': starts without the column');
    store = new Store(path);
    try {
      assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19, label);
      assert.deepEqual(store.db.prepare('SELECT rowid,* FROM assignments ORDER BY rowid').all().map(({ group_id, ...row }) => { assert.equal(group_id, null); return { ...row }; }), before, label + ': assignments are kept as they were');
      assert.deepEqual(store.desktopGroups(owner), []);
      assert.deepEqual(store.listDesktops(owner).map(desktop => [desktop.id, desktop.group, desktop.favorite]), [['pve-710', null, false], ['pve-711', null, true]]);
      assert.equal(store.clipboardPolicy(owner, 'pve-710').textToRemote, true, label + ': permissions survive');
      const [office] = store.replaceDesktopGroups(owner, [{ name: '업무' }]);
      assert.equal(store.setDesktopGroup(owner, 'pve-710', office.id), true);
    } finally { store.close(); }
    store = new Store(path);
    try {
      assert.deepEqual(store.desktopGroups(owner).map(group => group.name), ['업무'], label + ': the next start keeps the groups');
      assert.equal(store.listDesktops(owner).find(desktop => desktop.id === 'pve-710').group, store.desktopGroups(owner)[0].id);
    } finally { store.close(); }
  }
});

async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), sockets = [], errors = [];
  for (const name of ['alice', 'bobby', 'admin']) store.addUser({ name, role: name === 'admin' ? 'admin' : 'user', passwordHash: 'fixture-unused' });
  const inventory = [vm(710), vm(711), vm(712)];
  store.syncProxmox(inventory, 'fixture');
  enrollFixtureHosts(store, ['pve-710', 'pve-711', 'pve-712']);
  store.assign('alice', 'pve-710'); store.assign('alice', 'pve-711'); store.assign('bobby', 'pve-711'); store.assign('bobby', 'pve-712');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://isolated-pve.example', tokenId: 'fixture@pve!groups', secret: 'isolated-groups-token' }), 'fixture');
  const tokens = { alice: newToken(), aliceTablet: newToken(), bobby: newToken(), admin: newToken() };
  for (const name of Object.keys(tokens)) store.createSession(store.findUser(name === 'aliceTablet' ? 'alice' : name), tokenHash(tokens[name]), Date.now(), 3600000, 'client');
  const proxmox = { inventory: async (_config, { targets }) => targets.map(target => ({ ...inventory.find(row => row.vmid === target.vmid) })) };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, vmStateOptions: { syncMs: 2000 }, relayOptions: { heartbeatMs: 10000, authorizationMs: 10000 }, desktopEventOptions: { tickMs: 20, authorizationMs: 20, heartbeatMs: 1000 }, logger: { error: value => errors.push(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); assert.deepEqual(errors, [], 'no request failed unexpectedly'); });
  const base = 'http://127.0.0.1:' + server.address().port;
  const call = async (path, { method = 'GET', as = 'alice', data, raw, contentType = 'application/json', headers = {} } = {}) => {
    const response = await fetch(base + '/api/v1' + path, { method, headers: { ...(as ? { Authorization: 'Bearer ' + tokens[as] } : {}), ...(data !== undefined || raw !== undefined ? { 'Content-Type': contentType } : {}), ...headers }, body: raw ?? (data === undefined ? undefined : JSON.stringify(data)) });
    return { status: response.status, body: await response.json() };
  };
  const open = async as => {
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/events', { headers: { Authorization: 'Bearer ' + tokens[as], Origin: origin }, perMessageDeflate: false });
    ws.messages = []; ws.on('message', value => ws.messages.push(JSON.parse(value.toString('utf8')))); ws.on('error', () => {});
    await new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('unexpected-response', (_req, response) => { response.resume(); reject(new Error('upgrade_rejected')); }); });
    sockets.push(ws); return ws;
  };
  const until = async (predicate, message = 'fixture_timeout') => { const end = Date.now() + 3000; while (!predicate()) { assert.ok(Date.now() < end, message); await delay(5); } };
  const putGroups = (groups, options = {}) => call('/desktop-groups', { method: 'PUT', data: { groups }, ...options });
  const putGroup = (id, group, options = {}) => call(`/desktops/${id}/group`, { method: 'PUT', data: { group }, ...options });
  const list = async (as = 'alice') => (await call('/desktops', { as })).body;
  return { store, call, open, until, putGroups, putGroup, list, user: name => store.findUser(name).id };
}
const events = ws => ws.messages.filter(value => value.type === 'desktops.changed');

test('The desktop list carries the account groups in order and each desktop\'s group', async t => {
  const f = await fixture(t);
  const first = await f.list();
  assert.deepEqual(first.groups, [], 'no groups yet');
  assert.deepEqual(first.desktops.map(desktop => [desktop.id, desktop.group]), [['pve-710', null], ['pve-711', null]]);
  assert.deepEqual(first.features, { desktopOrganize: 1 });
  const made = await f.putGroups([{ name: '업무' }, { name: 'Lab' }]);
  assert.equal(made.status, 200);
  assert.deepEqual(made.body.groups.map(group => group.name), ['업무', 'Lab']);
  for (const group of made.body.groups) { assert.match(group.id, groupId); assert.deepEqual(Object.keys(group), ['id', 'name']); }
  assert.deepEqual(await f.putGroup('pve-711', made.body.groups[1].id), { status: 200, body: { group: made.body.groups[1].id } });
  const after = await f.list();
  assert.deepEqual(after.groups, made.body.groups);
  assert.deepEqual(after.desktops.map(desktop => [desktop.id, desktop.group]), [['pve-710', null], ['pve-711', made.body.groups[1].id]]);
  assert.equal((await f.call('/desktops/pve-711')).body.desktop.group, made.body.groups[1].id, 'the single desktop read carries it too');
  // Another account sees neither the groups nor the membership on the shared VM.
  const other = await f.list('bobby');
  assert.deepEqual(other.groups, []);
  assert.deepEqual(other.desktops.map(desktop => [desktop.id, desktop.group]), [['pve-711', null], ['pve-712', null]]);
});

test('PUT desktop-groups replaces the whole list: create, rename, reorder, delete (members ungrouped)', async t => {
  const f = await fixture(t);
  const [a, b] = (await f.putGroups([{ name: 'A' }, { name: 'B' }])).body.groups;
  assert.equal((await f.putGroup('pve-710', a.id)).status, 200); assert.equal((await f.putGroup('pve-711', b.id)).status, 200);
  // Rename and reorder keep the ids and the members; a new entry is a new group.
  const second = await f.putGroups([{ id: b.id, name: ' B renamed ' }, { name: 'C' }, { id: a.id, name: 'A' }]);
  assert.equal(second.status, 200);
  const [b2, c, a2] = second.body.groups;
  assert.deepEqual([b2, a2], [{ id: b.id, name: 'B renamed' }, { id: a.id, name: 'A' }]);
  assert.match(c.id, groupId); assert.notEqual(c.id, a.id); assert.notEqual(c.id, b.id);
  assert.deepEqual((await f.list()).groups, second.body.groups);
  assert.deepEqual((await f.list()).desktops.map(desktop => desktop.group), [a.id, b.id]);
  // Two names swapped in one request, and a change of case only.
  const swapped = await f.putGroups([{ id: a.id, name: 'B renamed' }, { id: b.id, name: 'a' }, { id: c.id, name: 'c' }]);
  assert.deepEqual(swapped.body.groups, [{ id: a.id, name: 'B renamed' }, { id: b.id, name: 'a' }, { id: c.id, name: 'c' }]);
  // Leaving a group out deletes it; its desktops become ungrouped, the others keep theirs.
  const third = await f.putGroups([{ id: c.id, name: 'c' }, { id: a.id, name: 'A' }]);
  assert.deepEqual(third.body.groups, [{ id: c.id, name: 'c' }, { id: a.id, name: 'A' }]);
  assert.deepEqual((await f.list()).desktops.map(desktop => [desktop.id, desktop.group]), [['pve-710', a.id], ['pve-711', null]]);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM desktop_groups WHERE id=?').get(b.id).n, 0);
  // The same list again changes nothing; an empty list deletes them all.
  assert.deepEqual((await f.putGroups([{ id: c.id, name: 'c' }, { id: a.id, name: 'A' }])).body.groups, third.body.groups);
  assert.deepEqual(await f.putGroups([]), { status: 200, body: { groups: [] } });
  assert.deepEqual((await f.list()).desktops.map(desktop => desktop.group), [null, null]);
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM assignments WHERE group_id IS NOT NULL').get().n, 0);
  // Positions follow the order sent.
  const ordered = (await f.putGroups(['3', '1', '2'].map(name => ({ name })))).body.groups;
  assert.deepEqual(f.store.db.prepare('SELECT name,position FROM desktop_groups WHERE user_id=? ORDER BY position').all(f.user('alice')).map(row => [row.name, row.position]), [['3', 0], ['1', 1], ['2', 2]]);
  assert.deepEqual(ordered.map(group => group.name), ['3', '1', '2']);
});

test('PUT desktop-groups limits and refusals change nothing', async t => {
  const f = await fixture(t);
  const [a] = (await f.putGroups([{ name: 'Keep' }])).body.groups;
  assert.equal((await f.putGroup('pve-710', a.id)).status, 200);
  const state = () => JSON.stringify([f.store.db.prepare('SELECT * FROM desktop_groups ORDER BY rowid').all(), f.store.db.prepare('SELECT user_id,desktop_id,group_id FROM assignments ORDER BY rowid').all()]);
  const unchanged = state();
  const refused = async (groups, status, code, options) => { const answer = await f.putGroups(groups, options); assert.equal(answer.status, status, JSON.stringify(groups)?.slice(0, 80)); if (code) assert.equal(answer.body.error.code, code); assert.equal(state(), unchanged, 'nothing changed: ' + code); };
  await refused(Array.from({ length: 31 }, (_, index) => ({ name: 'g' + index })), 400, 'too_many_groups');
  for (const name of ['', '   ', 'x'.repeat(41), 'a\nb', 'a\u0001b', 5, null]) await refused([{ id: a.id, name: 'Renamed' }, { name }], 400, 'invalid_group_name');
  await refused([{ id: a.id, name: 'Renamed' }, { name: 'renamed' }], 400, 'duplicate_group_name');
  await refused([{ id: a.id, name: 'Keep' }, { id: 'g_000000000000', name: 'Other' }], 400, 'unknown_group');
  await refused([{ id: 'not-a-group-id', name: 'Other' }], 400, 'unknown_group');
  for (const groups of [null, 'x', {}, [null], ['Keep'], [{ name: 'Keep', color: 'red' }], [{ id: 7, name: 'Keep' }], [{ id: a.id, name: 'A' }, { id: a.id, name: 'B' }]]) await refused(groups, 400, 'invalid_groups');
  assert.equal((await f.call('/desktop-groups', { method: 'PUT', data: { groups: [], extra: 1 } })).body.error.code, 'unexpected_field');
  assert.equal((await f.call('/desktop-groups', { method: 'PUT', data: {} })).body.error.code, 'invalid_groups');
  assert.equal((await f.call('/desktop-groups', { method: 'PUT', raw: '{"groups":' })).status, 400);
  assert.equal((await f.call('/desktop-groups', { method: 'PUT', raw: '[]' })).status, 400);
  assert.equal((await f.call('/desktop-groups', { method: 'PUT', raw: JSON.stringify({ groups: [{ name: 'x'.repeat(17000) }] }) })).status, 413);
  assert.equal((await f.call('/desktop-groups', { method: 'PUT', raw: '{"groups":[]}', contentType: 'text/plain' })).status, 415);
  for (const method of ['GET', 'POST', 'DELETE']) assert.equal((await f.call('/desktop-groups', { method, ...(method === 'POST' ? { data: { groups: [] } } : {}) })).status, 405, method);
  assert.equal((await f.putGroups([], { as: null })).status, 401);
  assert.equal((await f.putGroups([], { headers: { Origin: 'https://evil.example' } })).status, 403);
  assert.equal(state(), unchanged);
  // Exactly 30 groups with 40-character names fit, and the full body stays under the limit.
  const many = Array.from({ length: 30 }, (_, index) => ({ name: String(index).padStart(2, '0') + '😀'.repeat(38) }));
  const full = await f.putGroups(many);
  assert.equal(full.status, 200); assert.equal(full.body.groups.length, 30);
  assert.equal((await f.putGroups([...full.body.groups, { name: 'one more' }])).body.error.code, 'too_many_groups');
});

test('PUT desktops/:id/group sets and clears this account\'s group, 404 without leaking existence, strict body', async t => {
  const f = await fixture(t);
  const [a] = (await f.putGroups([{ name: 'A' }])).body.groups;
  const [bobbys] = (await f.putGroups([{ name: 'A' }], { as: 'bobby' })).body.groups;
  assert.notEqual(bobbys.id, a.id, 'the same name in another account is another group');
  assert.deepEqual(await f.putGroup('pve-711', a.id), { status: 200, body: { group: a.id } });
  assert.deepEqual(await f.putGroup('pve-711', a.id), { status: 200, body: { group: a.id } }, 'setting it again is fine');
  assert.equal((await f.list()).desktops.find(desktop => desktop.id === 'pve-711').group, a.id);
  assert.equal((await f.list('bobby')).desktops.find(desktop => desktop.id === 'pve-711').group, null, 'a shared VM keeps each account\'s own group');
  // Another account's group is refused, in both writes, and nothing changes.
  const refusedOther = await f.putGroup('pve-710', bobbys.id);
  assert.deepEqual([refusedOther.status, refusedOther.body.error.code], [400, 'unknown_group']);
  assert.equal((await f.putGroups([{ id: bobbys.id, name: 'Taken' }])).body.error.code, 'unknown_group');
  assert.deepEqual((await f.list('bobby')).groups, [{ id: bobbys.id, name: 'A' }]);
  assert.equal((await f.list()).desktops.find(desktop => desktop.id === 'pve-710').group, null);
  for (const group of ['g_000000000000', 'nonsense', '']) assert.equal((await f.putGroup('pve-710', group)).body.error.code, 'unknown_group', group);
  // Clearing.
  assert.deepEqual(await f.putGroup('pve-711', null), { status: 200, body: { group: null } });
  assert.equal((await f.list()).desktops.find(desktop => desktop.id === 'pve-711').group, null);
  // Unassigned, unknown and malformed desktops answer identically.
  const unassigned = await f.putGroup('pve-712', a.id), unknown = await f.putGroup('pve-999', a.id), malformed = await f.putGroup('bad%20id!', a.id);
  assert.equal(unassigned.status, 404); assert.equal(unassigned.body.error.code, 'desktop_not_found');
  assert.deepEqual(unknown, unassigned); assert.deepEqual(malformed, unassigned);
  assert.equal((await f.putGroup('pve-712', null, { as: 'admin' })).status, 404, 'an administrator is not assigned either');
  // Strict body.
  const put = data => f.call('/desktops/pve-710/group', { method: 'PUT', data });
  for (const data of [{}, { group: 5 }, { group: true }, { group: [] }, { group: { id: a.id } }]) assert.deepEqual([(await put(data)).status, (await put(data)).body.error.code], [400, 'invalid_group'], JSON.stringify(data));
  assert.equal((await put({ group: a.id, extra: 1 })).body.error.code, 'unexpected_field');
  assert.equal((await f.call('/desktops/pve-710/group', { method: 'PUT', raw: '{"group":' })).status, 400);
  assert.equal((await f.call('/desktops/pve-710/group', { method: 'PUT', raw: JSON.stringify({ group: 'x'.repeat(300) }) })).status, 413);
  assert.equal((await f.call('/desktops/pve-710/group', { method: 'PUT', raw: '{"group":null}', contentType: 'text/plain' })).status, 415);
  for (const method of ['GET', 'POST', 'DELETE']) assert.equal((await f.call('/desktops/pve-710/group', { method, ...(method === 'POST' ? { data: { group: null } } : {}) })).status, 405, method);
  assert.equal((await f.putGroup('pve-710', a.id, { as: null })).status, 401);
  assert.equal((await f.putGroup('pve-710', a.id, { headers: { Origin: 'https://evil.example' } })).status, 403);
  assert.equal((await f.list()).desktops.find(desktop => desktop.id === 'pve-710').group, null, 'no refused request wrote anything');
});

test('Group writes are rate limited per account', async t => {
  const f = await fixture(t);
  const [a] = (await f.putGroups([{ name: 'A' }])).body.groups;
  // One budget of 120 a minute for both writes: the list above, then 119 desktop moves.
  let last;
  for (let index = 0; index < 119; index++) last = await f.putGroup('pve-710', index % 2 ? null : a.id);
  assert.equal(last.status, 200);
  const limited = await f.putGroups([{ id: a.id, name: 'A' }]);
  assert.deepEqual([limited.status, limited.body.error.code], [429, 'too_many_group_attempts']);
  assert.equal((await f.putGroups([{ name: 'B' }], { as: 'bobby' })).status, 200, 'other accounts keep their budget');
});

test('Membership follows the assignment; groups follow the account and stay when a Host is removed', async t => {
  const f = await fixture(t);
  const alice = f.user('alice');
  const [a, b] = (await f.putGroups([{ name: 'A' }, { name: 'B' }])).body.groups;
  assert.equal((await f.putGroup('pve-710', a.id)).status, 200); assert.equal((await f.putGroup('pve-711', b.id)).status, 200);
  // Revoke takes the membership with the assignment; assigning again starts ungrouped.
  f.store.revoke('alice', 'pve-711');
  assert.equal((await f.putGroup('pve-711', b.id)).status, 404, 'a desktop that became unassigned');
  f.store.assign('alice', 'pve-711');
  assert.equal((await f.list()).desktops.find(desktop => desktop.id === 'pve-711').group, null, 'reassigning starts ungrouped');
  // Removing the Host deletes the assignments (and the membership) but no group.
  const host = f.store.db.prepare('SELECT id FROM hosts WHERE desktop_id=? AND revoked=0').get('pve-710').id;
  const removed = f.store.removeHostRegistration('pve-710', host, 'fixture', () => {});
  assert.equal(removed.removedAssignments, 1);
  assert.deepEqual(f.store.desktopGroups(alice), [a, b], 'groups stay when a Host is removed');
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM assignments WHERE desktop_id='pve-710'").get().n, 0);
  const after = await f.list();
  assert.deepEqual(after.groups, [a, b]);
  assert.deepEqual(after.desktops.map(desktop => [desktop.id, desktop.group]), [['pve-711', null]]);
  // A group row deleted by any path ungroups its members (foreign key).
  assert.equal(f.store.setDesktopGroup(alice, 'pve-711', a.id), true);
  f.store.db.prepare('DELETE FROM desktop_groups WHERE id=?').run(a.id);
  assert.equal(f.store.listDesktops(alice).find(desktop => desktop.id === 'pve-711').group, null);
  // A disabled account writes nothing; a deleted account takes its groups along.
  f.store.setUserEnabled('alice', false);
  assert.equal(f.store.replaceDesktopGroups(alice, [{ name: 'X' }]), null);
  assert.equal(f.store.setDesktopGroup(alice, 'pve-711', b.id), false);
  assert.deepEqual(f.store.desktopGroups(alice), [b]);
  f.store.deleteUser('alice', f.user('admin'));
  assert.equal(f.store.db.prepare('SELECT count(*) AS n FROM desktop_groups WHERE user_id=?').get(alice).n, 0);
  assert.equal(f.store.desktopGroups(f.user('bobby')).length, 0);
});

test('Group changes reach every live connection of the account only', async t => {
  const f = await fixture(t);
  const phone = await f.open('alice'), tablet = await f.open('aliceTablet'), other = await f.open('bobby');
  await f.until(() => [phone, tablet, other].every(ws => ws.messages.some(value => value.type === 'desktops.ready')));
  const [a] = (await f.putGroups([{ name: 'A' }])).body.groups;
  await f.until(() => events(phone).length === 1 && events(tablet).length === 1, 'a new group is pushed to both devices');
  // A rename moves no desktop but is a list change.
  assert.equal((await f.putGroups([{ id: a.id, name: 'A2' }])).status, 200);
  await f.until(() => events(phone).length === 2 && events(tablet).length === 2, 'a rename is pushed');
  assert.equal((await f.putGroup('pve-711', a.id)).status, 200);
  await f.until(() => events(phone).length === 3 && events(tablet).length === 3, 'a membership change is pushed');
  // The same values again change nothing visible: no push.
  await f.putGroups([{ id: a.id, name: 'A2' }]); await f.putGroup('pve-711', a.id); await delay(150);
  assert.equal(events(phone).length, 3);
  const [b] = (await f.putGroups([{ name: 'B' }, { id: a.id, name: 'A2' }])).body.groups;
  await f.until(() => events(phone).length === 4, 'a new order is pushed');
  assert.equal((await f.putGroups([{ id: a.id, name: 'A2' }, { id: b.id, name: 'B' }])).status, 200);
  await f.until(() => events(phone).length === 5, 'a reorder is pushed');
  await delay(100); assert.equal(events(other).length, 0, 'another account hears nothing');
});

test('Backup and restore carry each account\'s groups and memberships; an older backup restores ungrouped', async t => {
  const directory = temporaryDirectory(t), server = seedServer(directory), path = join(directory, 'snapshot.sqlite');
  const groupsBefore = server.store.desktopGroups(ids.member);
  assert.deepEqual(groupsBefore, [{ id: fixtureGroupId('a', 1), name: '업무 a' }, { id: fixtureGroupId('a', 2), name: 'Lab a' }]);
  await snapshotDatabase(server.store.db, path);
  sanitizeSnapshot(path, { now: 1794300999000 });
  const key = Buffer.from(server.vault.key); server.store.close();
  const rebuild = (source, name) => { const output = join(directory, name + '.sqlite'); return { output, summary: rebuildFromBackup({ source, work: join(directory, name + '-work.sqlite'), output, vaultKey: key }) }; };
  const { output, summary } = rebuild(path, 'restored');
  assert.equal(summary.migrated, false);
  assert.deepEqual(travellingRows(output).desktop_groups, travellingRows(path).desktop_groups);
  assert.equal(travellingRows(output).desktop_groups.length, 3);
  let store = new Store(output);
  try {
    assert.deepEqual(store.desktopGroups(ids.member), groupsBefore);
    assert.deepEqual(store.desktopGroups(ids.second), [{ id: fixtureGroupId('a', 3), name: '테스트' }]);
    assert.equal(store.db.prepare("SELECT group_id FROM assignments WHERE user_id=? AND desktop_id='pve-101'").get(ids.member).group_id, fixtureGroupId('a', 2));
    // The restored server keeps working on them.
    assert.equal(store.replaceDesktopGroups(ids.member, [{ id: fixtureGroupId('a', 2), name: 'Lab' }]).length, 1);
    assert.equal(store.db.prepare("SELECT group_id FROM assignments WHERE user_id=? AND desktop_id='pve-101'").get(ids.member).group_id, fixtureGroupId('a', 2));
  } finally { store.close(); }
  // A backup made before groups existed: migrated on restore, everything ungrouped.
  const older = join(directory, 'older.sqlite');
  copyFileSync(path, older);
  const db = new DatabaseSync(older);
  try { db.exec('PRAGMA foreign_keys=OFF; DROP INDEX assignments_group; ALTER TABLE assignments DROP COLUMN group_id; DROP TABLE desktop_groups;'); } finally { db.close(); }
  const fromOlder = rebuild(older, 'from-older');
  assert.equal(fromOlder.summary.migrated, true);
  store = new Store(fromOlder.output);
  try {
    assert.deepEqual(store.desktopGroups(ids.member), []);
    assert.equal(store.db.prepare('SELECT count(*) AS n FROM assignments WHERE group_id IS NOT NULL').get().n, 0);
    assert.equal(store.db.prepare('SELECT count(*) AS n FROM assignments').get().n, 4, 'assignments come back');
  } finally { store.close(); }
});
