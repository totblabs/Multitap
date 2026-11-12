// Desktop organization: Proxmox tags/pool, per-account favorite and last use.
// Real loopback HTTP/WebSocket routing with synthetic Proxmox/Host identities.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { DatabaseSync } from 'node:sqlite';
import { mkdtemp, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { WebSocket } from 'ws';
import { parseHTML } from 'linkedom';
import { Store } from '../src/store.mjs';
import { Store as Store18 } from './fixtures/mfa-store18.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { Proxmox } from '../src/proxmox.mjs';
import { normalizeTags, normalizePool } from '../src/desktop-organize.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { enrollFixtureHosts } from './fixtures/admin-assignment-input.mjs';

const origin = 'https://organize-fixture.example';
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));
const machineUuid = vmid => vmid === 710 ? '12345678-1234-1234-1234-123456789abc' : '12345678-1234-4234-8234-' + String(vmid).padStart(12, '0');
const vm = (vmid, extra = {}) => ({ vmid, node: 'fixture-node', name: `VM${vmid}`, status: 'running', machineUuid: machineUuid(vmid), ...extra });

test('Tags are split on ; , and spaces, validated, de-duplicated case-insensitively and bounded', () => {
  assert.deepEqual(normalizeTags('office;Dev,dev  test+1;a.b;_x;-bad;bad tag!;'), ['office', 'Dev', 'test+1', 'a.b', '_x', 'bad']);
  assert.deepEqual(normalizeTags('Prod;PROD;prod'), ['Prod']);
  assert.deepEqual(normalizeTags(''), []);
  assert.deepEqual(normalizeTags(undefined), []);
  assert.deepEqual(normalizeTags(null), []);
  assert.deepEqual(normalizeTags(42), []);
  assert.deepEqual(normalizeTags(';;; ,'), []);
  assert.deepEqual(normalizeTags('한글;ok;spécial'), ['ok'], 'only Proxmox-valid ASCII tags');
  assert.deepEqual(normalizeTags(`${'a'.repeat(64)};${'b'.repeat(65)}`), ['a'.repeat(64)]);
  const many = normalizeTags(Array.from({ length: 80 }, (_, index) => `t${index}`).join(';'));
  assert.equal(many.length, 32); assert.equal(many[0], 't0'); assert.equal(many[31], 't31');
  assert.deepEqual(normalizeTags(['a', 5, 'A', 'b c']), ['a'], 'arrays are validated entry by entry');
});

test('Pool is a trimmed string of at most 64 characters or null', () => {
  assert.equal(normalizePool(' Lab-pool '), 'Lab-pool');
  assert.equal(normalizePool(''), null); assert.equal(normalizePool('   '), null);
  assert.equal(normalizePool(undefined), null); assert.equal(normalizePool(5), null);
  assert.equal(normalizePool('x'.repeat(65)), null); assert.equal(normalizePool('a\u0000b'), null);
});

test('Proxmox.inventory reads tags and pool only from exact registered configs (none, messy, no pool)', async () => {
  const proxmox = new Proxmox();
  const rows = new Map([[700,{name:'a',tags:'Office;office;dev',pool:'Lab'}],[701,{name:'b'}],[702,{name:'c',tags:';;x y',pool:''}]]),calls=[];
  proxmox.call = async(_config,path,method='GET')=>{
    calls.push(path);assert.equal(method,'GET');assert.match(path,/^\/nodes\/n\/qemu\/70[0-2]\/(?:config\?current=1|status\/current)$/);
    const vmid=Number(path.split('/')[4]);
    return path.endsWith('/status/current')?{status:vmid===700?'running':'stopped'}:{...rows.get(vmid),smbios1:'uuid='+machineUuid(vmid)};
  };
  const vms = await proxmox.inventory({}, { enrich: false,targets:[700,701,702].map(vmid=>({node:'n',vmid,expectedUuid:machineUuid(vmid)})) });
  assert.deepEqual(vms.map(item => [item.vmid, item.tags, item.pool]), [[700, ['Office', 'dev'], 'Lab'], [701, [], null], [702, ['x', 'y'], null]]);
  assert.equal(calls.some(path=>path.startsWith('/cluster/')||path.includes('/703/')),false);
});

test('Sync replaces tags on success, keeps them for VMs a result lacks or a result without the field, and re-creation replaces', () => {
  const store = new Store(':memory:');
  try {
    store.syncProxmox([vm(710, { tags: ['a', 'b'], pool: 'P1' }), vm(711, { tags: ['keep'], pool: null })], 'fixture');
    const read = id => store.inventory().desktops.find(d => d.pve_vmid === id);
    assert.deepEqual(read(710).pve_tags, ['a', 'b']); assert.equal(read(710).pve_pool, 'P1');
    // Rename in Proxmox: the next successful result replaces.
    store.syncProxmox([vm(710, { tags: ['a2'], pool: null }), vm(711, { tags: ['keep'] })], 'fixture');
    assert.deepEqual(read(710).pve_tags, ['a2']); assert.equal(read(710).pve_pool, null);
    // VM 711 is missing from a (partial) result: its values stay; 710 without a tags/pool field keeps too.
    store.syncProxmox([vm(710)], 'fixture');
    assert.deepEqual(read(711).pve_tags, ['keep']); assert.deepEqual(read(710).pve_tags, ['a2']);
    // A VM that now has no tags at all clears them (empty is a result).
    store.syncProxmox([vm(710, { tags: [], pool: null }), vm(711, { tags: ['keep'] })], 'fixture');
    assert.deepEqual(read(710).pve_tags, []);
    // Deleted and re-created with the same id: the new values win.
    store.syncProxmox([vm(711, { tags: ['keep'] })], 'fixture');
    store.syncProxmox([vm(710, { tags: ['reborn'], pool: 'P2' }), vm(711, { tags: ['keep'] })], 'fixture');
    assert.deepEqual(read(710).pve_tags, ['reborn']); assert.equal(read(710).pve_pool, 'P2');
    // Hand-written or damaged stored text never breaks reading.
    store.db.prepare("UPDATE desktops SET pve_tags='not json' WHERE pve_vmid=711").run();
    assert.deepEqual(read(711).pve_tags, []);
  } finally { store.close(); }
});

test('Schema 14 databases migrate: tags empty, favorite false, last use null; nothing else changes', async t => {
  const directory = await mkdtemp(join(tmpdir(), 'multitap-organize-migration-'));
  t.after(() => rm(directory, { recursive: true }));
  const path = join(directory, 'broker.sqlite');
  let store = new Store18(path);
  const owner = store.addUser({ name: 'fixture-user', passwordHash: 'retained-password-fixture' });
  store.syncProxmox([vm(710)], 'fixture');
  // The client projection needs an approved Host, including after migration.
  enrollFixtureHosts(store, ['pve-710']);
  store.assign('fixture-user', 'pve-710', 'fixture');
  store.updateAssignmentPolicy('fixture-user', 'pve-710', { clipboardPolicy: { textToRemote: true, textToLocal: false, filesToRemote: false, filesToLocal: false } });
  store.close();
  const old = new DatabaseSync(path);
  old.exec('DROP TABLE session_activity;DROP TABLE session_end_reasons;ALTER TABLE assignments DROP COLUMN sound_card_add;ALTER TABLE assignments DROP COLUMN favorite;ALTER TABLE assignments DROP COLUMN last_used_at;ALTER TABLE desktops DROP COLUMN pve_tags;ALTER TABLE desktops DROP COLUMN pve_pool;DROP INDEX sessions_windows_owner;DROP INDEX sessions_replaced;ALTER TABLE sessions DROP COLUMN replaced_at;ALTER TABLE sessions DROP COLUMN issuer;PRAGMA user_version=14;');
  old.close();
  store = new Store(path);
  try {
    assert.equal(store.db.prepare('PRAGMA user_version').get().user_version, 19);
    const [desktop] = store.listDesktops(owner);
    assert.deepEqual([desktop.id, desktop.tags, desktop.pool, desktop.favorite, desktop.lastUsedAt], ['pve-710', [], null, false, null]);
    assert.equal(store.clipboardPolicy(owner, 'pve-710').textToRemote, true, 'permissions survive');
    assert.equal(store.setFavorite(owner, 'pve-710', true), true);
  } finally { store.close(); }
  store = new Store(path);
  try { assert.equal(store.listDesktops(owner)[0].favorite, true, 'reopening an up-to-date database keeps the data'); } finally { store.close(); }
});

const capabilities = { service: true, interactiveSession: true, streaming: true, virtualDisplayReady: true };
async function fixture(t) {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32)), sockets = [];
  for (const name of ['alice', 'bobby', 'admin']) store.addUser({ name, role: name === 'admin' ? 'admin' : 'user', passwordHash: 'fixture-unused' });
  let inventory = [vm(710, { tags: ['office'], pool: 'Lab' }), vm(711), vm(712)];
  store.syncProxmox(inventory, 'fixture');
  enrollFixtureHosts(store,['pve-711','pve-712']);
  store.assign('alice', 'pve-710'); store.assign('alice', 'pve-711'); store.assign('bobby', 'pve-711'); store.assign('bobby', 'pve-712');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://isolated-pve.example', tokenId: 'fixture@pve!organize', secret: 'isolated-organize-token' }), 'fixture');
  const tokens = { alice: newToken(), aliceTablet: newToken(), bobby: newToken(), admin: newToken(), host: newToken() }, code = newToken();
  for (const name of ['alice', 'aliceTablet', 'bobby', 'admin']) store.createSession(store.findUser(name === 'aliceTablet' ? 'alice' : name), tokenHash(tokens[name]), Date.now(), 3600000, 'client');
  store.issueEnrollment('pve-710', '12345678-1234-1234-1234-123456789abc', tokenHash(code), 'fixture');
  store.consumeEnrollment(tokenHash(code), '12345678-1234-1234-1234-123456789abc', { hostname: 'Fixture', version: '9.0.0', tokenHash: tokenHash(tokens.host) });
  let now = Date.now();
  const proxmox = { inventory: async (_config,{targets}) => {assert.ok(Array.isArray(targets));return targets.map(target=>{assert.deepEqual(target,{node:'fixture-node',vmid:target.vmid,expectedUuid:machineUuid(target.vmid)});const row=inventory.find(row=>row.vmid===target.vmid);assert.ok(row);return{...row};});} };
  const server = await buildServer({ store, vault, proxmox, publicOrigin: origin, powerOptions: { now: () => now }, vmStateOptions: { syncMs: 2000 }, relayOptions: { heartbeatMs: 10000, authorizationMs: 10000 }, desktopEventOptions: { tickMs: 20, authorizationMs: 20, heartbeatMs: 1000 }, logger: { error: value => assert.fail(value) } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  t.after(async () => { for (const ws of sockets) ws.terminate(); await new Promise(resolve => server.close(resolve)); store.close(); });
  const base = 'http://127.0.0.1:' + server.address().port;
  const open = async (path, as, hostSocket = false) => {
    const ws = new WebSocket(base.replace('http:', 'ws:') + path, { headers: { Authorization: (hostSocket ? 'Host ' : 'Bearer ') + tokens[as], ...(hostSocket ? {} : { Origin: origin }) }, perMessageDeflate: false });
    ws.messages = []; ws.on('message', value => ws.messages.push(JSON.parse(value.toString('utf8')))); ws.on('error', () => {});
    await new Promise((resolve, reject) => { ws.once('open', resolve); ws.once('unexpected-response', (_req, response) => { response.resume(); reject(new Error('upgrade_rejected')); }); });
    sockets.push(ws); return ws;
  };
  const call = async (path, { method = 'GET', as = 'alice', data, raw, contentType = 'application/json', headers = {} } = {}) => {
    const response = await fetch(base + '/api/v1' + path, { method, headers: { ...(as ? { Authorization: 'Bearer ' + tokens[as] } : {}), ...(data !== undefined || raw !== undefined ? { 'Content-Type': contentType } : {}), ...headers }, body: raw ?? (data === undefined ? undefined : JSON.stringify(data)) });
    return { status: response.status, body: await response.json() };
  };
  const until = async (predicate, message = 'fixture_timeout') => { const end = Date.now() + 3000; while (!predicate()) { assert.ok(Date.now() < end, message); await delay(5); } };
  const host = await open('/api/v1/hosts/channel', 'host', true); host.send(JSON.stringify({ type: 'host.capabilities', capabilities }));
  await until(() => host.messages.some(v => v.type === 'host.capabilities.ack'));
  return { store, call, open, until, setInventory: value => { inventory = value; }, advance: () => { now += 60000; }, tokens };
}
const events = ws => ws.messages.filter(value => value.type === 'desktops.changed');

test('Desktop list carries tags, pool, favorite and lastUsedAt, plus the desktopOrganize feature', async t => {
  const f = await fixture(t);
  const list = await f.call('/desktops');
  assert.equal(list.status, 200); assert.deepEqual(list.body.features, { desktopOrganize: 1 });
  const first = list.body.desktops.find(d => d.id === 'pve-710'), second = list.body.desktops.find(d => d.id === 'pve-711');
  assert.deepEqual([first.tags, first.pool, first.favorite, first.lastUsedAt], [['office'], 'Lab', false, null]);
  assert.deepEqual([second.tags, second.pool, second.favorite, second.lastUsedAt], [[], null, false, null], 'a VM without tags');
  const single = await f.call('/desktops/pve-710');
  assert.deepEqual([single.body.desktop.tags, single.body.desktop.favorite], [['office'], false]);
});

test('PUT favorite: only the assigned account, strict body, per account, 404 without leaking existence', async t => {
  const f = await fixture(t);
  const put = (id, data, options = {}) => f.call(`/desktops/${id}/favorite`, { method: 'PUT', data, ...options });
  assert.deepEqual(await put('pve-710', { favorite: true }), { status: 200, body: { favorite: true } });
  assert.equal((await f.call('/desktops')).body.desktops.find(d => d.id === 'pve-710').favorite, true);
  assert.equal((await f.call('/desktops', { as: 'bobby' })).body.desktops.some(d => d.id === 'pve-710'), false);
  // Another account's mark on the shared VM is independent.
  assert.equal((await put('pve-711', { favorite: true }, { as: 'bobby' })).status, 200);
  assert.equal((await f.call('/desktops')).body.desktops.find(d => d.id === 'pve-711').favorite, false);
  assert.equal((await f.call('/desktops', { as: 'bobby' })).body.desktops.find(d => d.id === 'pve-711').favorite, true);
  // Unassigned and unknown ids answer identically.
  const unassigned = await put('pve-712', { favorite: true }), unknown = await put('pve-999', { favorite: true }), malformed = await put('bad%20id!', { favorite: true });
  assert.equal(unassigned.status, 404); assert.deepEqual(unassigned, unknown); assert.deepEqual(unassigned, malformed);
  assert.equal(unassigned.body.error.code, 'desktop_not_found');
  assert.equal((await put('pve-712', { favorite: true }, { as: 'admin' })).status, 404, 'an administrator is not assigned either');
  // Strict body.
  for (const data of [{}, { favorite: 'true' }, { favorite: 1 }, { favorite: null }, { favorite: true, extra: 1 }, { Favorite: true }]) assert.equal((await put('pve-710', data)).status, 400, JSON.stringify(data));
  assert.equal((await put('pve-710', undefined, { raw: '[true]' })).status, 400);
  assert.equal((await put('pve-710', undefined, { raw: '{"favorite":' })).status, 400);
  assert.equal((await put('pve-710', undefined, { raw: 'x'.repeat(400) })).status, 413);
  assert.equal((await put('pve-710', undefined, { raw: '{"favorite":true}', contentType: 'text/plain' })).status, 415);
  assert.equal((await f.call('/desktops/pve-710/favorite', { method: 'POST', data: { favorite: true } })).status, 405);
  assert.equal((await f.call('/desktops/pve-710/favorite', { method: 'GET' })).status, 405);
  assert.equal((await put('pve-710', { favorite: true }, { as: null })).status, 401);
  assert.equal((await put('pve-710', { favorite: true }, { headers: { Origin: 'https://evil.example' } })).status, 403);
  // Unmarking.
  assert.deepEqual(await put('pve-710', { favorite: false }), { status: 200, body: { favorite: false } });
  assert.equal((await f.call('/desktops')).body.desktops.find(d => d.id === 'pve-710').favorite, false);
});

test('PUT favorite is rate limited per account', async t => {
  const f = await fixture(t);
  let last;
  for (let index = 0; index < 61; index++) last = await f.call('/desktops/pve-710/favorite', { method: 'PUT', data: { favorite: index % 2 === 0 } });
  assert.equal(last.status, 429);
  assert.equal((await f.call('/desktops/pve-711/favorite', { method: 'PUT', as: 'bobby', data: { favorite: true } })).status, 200, 'other accounts keep their budget');
});

test('Favorite follows the assignment: revoke removes it, reassign starts fresh, a disabled account cannot write', async t => {
  const f = await fixture(t);
  const put = (as = 'alice') => f.call('/desktops/pve-711/favorite', { method: 'PUT', as, data: { favorite: true } });
  assert.equal((await put()).status, 200);
  f.store.revoke('alice', 'pve-711');
  assert.equal((await put()).status, 404, 'favorite on a VM that became unassigned');
  f.store.assign('alice', 'pve-711');
  const row = (await f.call('/desktops')).body.desktops.find(d => d.id === 'pve-711');
  assert.deepEqual([row.favorite, row.lastUsedAt], [false, null], 'reassigning starts fresh');
  assert.equal(f.store.db.prepare("SELECT count(*) AS n FROM assignments WHERE favorite=1").get().n, 0);
  assert.equal(f.store.setFavorite(f.store.findUser('bobby').id, 'pve-711', true), true);
  f.store.setUserEnabled('bobby', false);
  assert.equal(f.store.setFavorite(f.store.findUser('bobby').id, 'pve-711', false), false, 'disabled accounts write nothing');
});

test('Favorite changes reach every live connection of the account only; concurrent toggles end on one winner', async t => {
  const f = await fixture(t);
  const phone = await f.open('/api/v1/events', 'alice'), tablet = await f.open('/api/v1/events', 'aliceTablet'), other = await f.open('/api/v1/events', 'bobby');
  await f.until(() => [phone, tablet, other].every(ws => ws.messages.some(v => v.type === 'desktops.ready')));
  assert.equal((await f.call('/desktops/pve-710/favorite', { method: 'PUT', data: { favorite: true } })).status, 200);
  await f.until(() => events(phone).length === 1 && events(tablet).length === 1, 'both devices are told');
  await delay(100); assert.equal(events(other).length, 0, 'another account hears nothing');
  assert.equal(events(tablet).length, 1);
  // Setting the value it already has changes nothing visible: no push.
  await f.call('/desktops/pve-710/favorite', { method: 'PUT', data: { favorite: true } }); await delay(100);
  assert.equal(events(phone).length, 1);
  // Two devices toggle at once: both succeed, the last write wins, both are told.
  const [a, b] = await Promise.all([
    f.call('/desktops/pve-710/favorite', { method: 'PUT', as: 'alice', data: { favorite: false } }),
    f.call('/desktops/pve-710/favorite', { method: 'PUT', as: 'aliceTablet', data: { favorite: true } }),
  ]);
  assert.deepEqual([a.status, b.status], [200, 200]);
  const final = (await f.call('/desktops')).body.desktops.find(d => d.id === 'pve-710').favorite;
  assert.equal(typeof final, 'boolean');
  assert.equal((await f.call('/desktops', { as: 'aliceTablet' })).body.desktops.find(d => d.id === 'pve-710').favorite, final, 'every device reads the same winner');
  if (final === false) await f.until(() => events(phone).length === 2 && events(tablet).length === 2, 'a real change is pushed to both');
});

test('A Proxmox tag or pool change reaches the account of that VM by push, without a client poll or admin sync', async t => {
  const f = await fixture(t);
  const phone = await f.open('/api/v1/events', 'alice'), other = await f.open('/api/v1/events', 'bobby');
  await f.until(() => phone.messages.some(v => v.type === 'desktops.ready') && other.messages.some(v => v.type === 'desktops.ready'));
  f.setInventory([vm(710, { tags: ['office', 'new'], pool: 'Lab' }), vm(711), vm(712)]); f.advance();
  await f.until(() => events(phone).length === 1, 'tag change is pushed');
  await delay(100); assert.equal(events(other).length, 0, 'bobby has no VM with a changed organization');
  assert.deepEqual((await f.call('/desktops')).body.desktops.find(d => d.id === 'pve-710').tags, ['office', 'new']);
  f.setInventory([vm(710, { tags: ['office', 'new'], pool: 'Other' }), vm(711), vm(712)]); f.advance();
  await f.until(() => events(phone).length === 2, 'pool change is pushed');
});

test('lastUsedAt is set when a stream grant is issued, never for refused connects (RDP included)', async t => {
  const f = await fixture(t);
  const lastUsed = async (as = 'alice') => (await f.call('/desktops', { as })).body.desktops.find(d => d.id === 'pve-710')?.lastUsedAt;
  assert.equal(await lastUsed(), null);
  assert.equal((await f.call('/desktops/pve-710/connect', { method: 'POST', as: 'bobby', data: { mode: 'stream' } })).status, 404, 'unassigned account');
  assert.equal((await f.call('/desktops/pve-710/connect', { method: 'POST', data: { mode: 'vnc' } })).status, 400, 'invalid mode');
  assert.equal(await lastUsed(), null, 'failed authorizations are not use');
  const before = Date.now();
  const grant = await f.call('/desktops/pve-710/connect', { method: 'POST', data: { mode: 'stream' } });
  assert.equal(grant.status, 200);
  const used = await lastUsed();
  assert.ok(Date.parse(used) >= before && Date.parse(used) <= Date.now());
  assert.equal(await f.call('/desktops/pve-710/connect', { method: 'POST', data: { mode: 'stream' } }).then(r => r.status), 409, 'busy: a refused second grant');
  assert.equal(await lastUsed(), used, 'the refused attempt did not move it');
  await f.call('/sessions/' + grant.body.sessionId, { method: 'DELETE' });
  await delay(5);
  // RDP is not offered: its refused connect is not use either.
  const rdp = await f.call('/desktops/pve-710/connect', { method: 'POST', data: { mode: 'rdp' } });
  assert.equal(rdp.status, 409); assert.equal(rdp.body.error.code, 'rdp_not_supported');
  assert.equal(await lastUsed(), used, 'the refused RDP attempt did not move it');
});

test('Admin console: tag chips, tooltip, and the VM search matching tags and pool', () => {
  const window = parseHTML('<html><body></body></html>'), { document } = window;
  const previous = [globalThis.document, globalThis.Node]; globalThis.document = document; globalThis.Node = window.Node;
  return (async () => {
    try {
      const { tagChips } = await import('../admin/js/desktops.js');
      const { vmSearchText, vmTags, vmPool, tagsHint } = await import('../admin/js/labels.js');
      const desktop = { pve_vmid: 710, name: 'Design PC', pve_node: 'pve-a', os: 'Windows 11', pve_tags: ['Office', 'dev'], pve_pool: 'Lab' };
      const chips = tagChips(desktop);
      assert.deepEqual([...chips.children].map(node => node.textContent), ['Lab', 'Office', 'dev']);
      assert.match(chips.getAttribute('title'), /Proxmox/); assert.equal(chips.getAttribute('title'), tagsHint);
      assert.equal(tagChips({ pve_vmid: 711, name: 'plain', pve_tags: [], pve_pool: null }), null, 'no tags, no chips');
      assert.equal(tagChips({ pve_vmid: 712, name: 'old broker row' }), null, 'rows without the fields');
      assert.deepEqual(vmTags({ pve_tags: ['a', 5, 'b'] }), ['a', 'b']); assert.equal(vmPool({ pve_pool: null }), '');
      for (const query of ['office', 'DEV', 'lab', '710', 'design', 'pve-a']) assert.ok(vmSearchText(desktop).includes(query.toLowerCase()), query);
      assert.equal(vmSearchText(desktop).includes('finance'), false);
      assert.ok(vmSearchText({ pve_vmid: 1, name: 'x' }).includes('x'), 'a row without tags still searches');
    } finally { [globalThis.document, globalThis.Node] = previous; }
  })();
});

test('Admin inventory exposes tags as an array and the pool', async t => {
  const f = await fixture(t);
  const row = f.store.inventory().desktops.find(d => d.id === 'pve-710');
  assert.deepEqual(row.pve_tags, ['office']); assert.equal(row.pve_pool, 'Lab');
});

test('A VM whose Proxmox QEMU guest agent option is off carries qemuAgentOff', async t => {
  const f = await fixture(t);
  f.setInventory([vm(710, { tags: ['office'], pool: 'Lab' }), vm(711), vm(712)].map(row => ({ ...row, qemuAgent: row.vmid !== 710 })));
  f.advance();
  const list = await f.call('/desktops');
  assert.equal(list.status, 200);
  const off = list.body.desktops.find(d => d.id === 'pve-710'), on = list.body.desktops.find(d => d.id === 'pve-711');
  assert.equal(off.qemuAgentOff, true);
  assert.equal(Object.hasOwn(on, 'qemuAgentOff'), false, 'an enabled option adds nothing');
  f.setInventory([vm(710, { tags: ['office'], pool: 'Lab' }), vm(711), vm(712)]);
  f.advance();
  const unknown = await f.call('/desktops');
  assert.equal(Object.hasOwn(unknown.body.desktops.find(d => d.id === 'pve-710'), 'qemuAgentOff'), false, 'an unread option adds nothing');
});