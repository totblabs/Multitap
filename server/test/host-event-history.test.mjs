// Tuning #8: a rolling, bounded Host event history per Host on the broker.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { randomBytes } from 'node:crypto';
import { once } from 'node:events';
import { WebSocket } from 'ws';
import { Store } from '../src/store.mjs';
import { Vault } from '../src/vault.mjs';
import { buildServer } from '../src/http.mjs';
import { newToken, tokenHash } from '../src/security.mjs';
import { safeDiagnosticReport, recordHostEventHistory, hostEventHistory, hostEventHistoryLimit, hostEventHistoryMs } from '../src/host-diagnostics.mjs';
import { clientEntry, desktopHostList, hostEntries } from '../admin/js/timeline.js';

const at = ms => new Date(Date.UTC(2026, 10, 2) + ms).toISOString();
const event = (ms, values = { action: 250, stage: 31, ok: true, items: ms }) => ({ time: at(ms), event: 'hostAction', values });
const report = (events, version = '0.2.146') => safeDiagnosticReport({ protocol: 'host-diagnostics-v1', source: 'host-service', version, collectedAt: at(0), interactiveSession: true, consoleSession: true, captureAvailable: true,
  display: { observed: true, observationAgeMs: 0, installed: true, deviceReady: true, owned: true, activeDisplay: true, configurationPending: false, recoveryRequired: false, rebootRequired: false, remoteSession: false, monitorCount: 0, width: 0, height: 0, reason: '' },
  native: { available: false }, events });

function enrolled(store, vmids) {
  store.syncProxmox(vmids.map(vmid => ({ vmid, node: 'fixture', name: 'VM' + vmid, status: 'running' })), 'fixture');
  return vmids.map((vmid, index) => {
    const code = newToken(), machine = `00000000-1234-1234-1234-00000000000${index}`;
    store.issueEnrollment('pve-' + vmid, machine, tokenHash(code), 'fixture');
    store.consumeEnrollment(tokenHash(code), machine, { hostname: 'fixture', version: '0.2.146', tokenHash: tokenHash(newToken()) });
    return store.db.prepare('SELECT id FROM hosts WHERE desktop_id=?').get('pve-' + vmid).id;
  });
}

test('Each report merges into its Host history once per event, whatever the field order', () => {
  const store = new Store(':memory:');
  try {
    const [first, second] = enrolled(store, [149, 170]), now = Date.now();
    recordHostEventHistory(store, first, report([event(1), event(2)]), now);
    // The next report repeats event 2 (fields in another order) beside a new one.
    recordHostEventHistory(store, first, report([{ time: at(2), event: 'hostAction', values: { items: 2, ok: true, stage: 31, action: 250 } }, event(3)], '0.2.147'), now + 5000);
    recordHostEventHistory(store, second, report([event(2)]), now);
    const history = hostEventHistory(store, first, now + 5000);
    assert.deepEqual(history.map(row => row.values.items), [3, 2, 1], 'newest first, each once');
    assert.equal(history[0].version, '0.2.147');
    assert.equal(history[1].version, '0.2.146', 'a repeat keeps its first receipt');
    assert.deepEqual(Object.keys(history[0]).sort(), ['event', 'receivedAt', 'time', 'values', 'version']);
    assert.equal(hostEventHistory(store, second, now).length, 1, 'Hosts are kept apart');
  } finally { store.close(); }
});

test('History is bounded: the newest 500 per Host and 7 days', () => {
  const store = new Store(':memory:');
  try {
    const [host, other] = enrolled(store, [149, 170]), now = Date.now();
    recordHostEventHistory(store, other, report([event(1)]), now);
    for (let batch = 0; batch < 66; batch++) recordHostEventHistory(store, host, report(Array.from({ length: 8 }, (_, i) => event(batch * 8 + i))), now);
    const count = id => store.db.prepare('SELECT count(*) AS n FROM host_diagnostic_events WHERE host_id=?').get(id).n;
    assert.equal(count(host), hostEventHistoryLimit);
    const kept = hostEventHistory(store, host, now);
    assert.equal(kept.length, 500);
    assert.equal(kept[0].values.items, 527, 'the newest stays');
    assert.equal(kept.at(-1).values.items, 28, 'the oldest 28 went');
    assert.equal(count(other), 1, "one Host's volume never evicts another's");
    // Older than 7 days: hidden at once, deleted on the next merge.
    assert.equal(hostEventHistory(store, other, now + hostEventHistoryMs + 1).length, 0);
    recordHostEventHistory(store, host, report([event(9999)]), now + hostEventHistoryMs + 1);
    assert.equal(count(other), 0);
    assert.equal(count(host), 1);
  } finally { store.close(); }
});

test('Stored rows are re-projected on read: unknown fields and damaged rows never leave', () => {
  const store = new Store(':memory:');
  try {
    const [host] = enrolled(store, [149]), now = Date.now();
    recordHostEventHistory(store, host, report([event(1)]), now);
    const insert = store.db.prepare('INSERT INTO host_diagnostic_events(host_id,received_at,version,row) VALUES(?,?,?,?)');
    insert.run(host, now, '0.2.146', JSON.stringify({ time: at(2), event: 'hostAction', values: { action: 253, ok: false, token: 'DO_NOT_SEND', name: 'secret.txt' } }));
    insert.run(host, now, '0.2.146', '{not json');
    insert.run(host, now, 'DO_NOT_SEND', JSON.stringify(event(3)));
    insert.run(host, now, '0.2.146', JSON.stringify({ time: at(4), event: 'DO_NOT_SEND', values: {} }));
    const history = hostEventHistory(store, host, now);
    assert.deepEqual(history.map(row => row.time), [at(2), at(1)]);
    assert.deepEqual(history[0].values, { action: 253, ok: false });
    assert.equal(JSON.stringify(history).includes('DO_NOT_SEND'), false);
  } finally { store.close(); }
});

test('Accepted reports reach the administrator diagnostics view as history beside the latest report', async t => {
  const store = new Store(':memory:'), vault = new Vault(randomBytes(32));
  store.addUser({ name: 'admin', passwordHash: 'fixture', role: 'admin' });store.addUser({ name: 'alice', passwordHash: 'fixture', role: 'user' });
  const adminToken = newToken(), userToken = newToken();
  store.createSession(store.findUser('admin'), tokenHash(adminToken), Date.now(), 60000, 'admin');store.createSession(store.findUser('alice'), tokenHash(userToken), Date.now(), 60000);
  const vms = [{ vmid: 149, node: 'fixture', name: 'TESTER', status: 'running' }];store.syncProxmox(vms, 'fixture');store.assign('alice', 'pve-149');
  store.saveSetting('proxmox', vault.seal({ endpoint: 'https://pve.invalid', tokenId: 'fixture@pve!fixture', secret: 'fixture' }), 'fixture');
  const code = newToken(), hostToken = newToken(), machine = '00000000-1234-1234-1234-000000000001';
  store.issueEnrollment('pve-149', machine, tokenHash(code), 'fixture');store.consumeEnrollment(tokenHash(code), machine, { hostname: 'fixture', version: '0.2.146', tokenHash: tokenHash(hostToken) });
  const server = await buildServer({ store, vault, publicOrigin: 'https://diagnostics.invalid', proxmox: { inventory: async () => vms }, logger: { error: () => assert.fail('Unexpected server error') } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));t.after(async () => { await new Promise(resolve => server.close(resolve));store.close(); });
  const base = `http://127.0.0.1:${server.address().port}`;
  const ws = new WebSocket(base.replace('http:', 'ws:') + '/api/v1/hosts/channel', { headers: { Authorization: 'Host ' + hostToken, 'X-Multitap-Host-Protocol': 'capabilities-v1' } });await once(ws, 'open');t.after(() => ws.close());
  const send = async value => { const receive = once(ws, 'message');ws.send(JSON.stringify(value));const [bytes] = await receive;return JSON.parse(bytes.toString()); };
  const api = async (who = 'admin') => { const response = await fetch(base + '/api/v1/admin/desktops/pve-149/diagnostics', { headers: who === 'admin' ? { Cookie: '__Secure-multitap-admin=' + adminToken } : { Authorization: 'Bearer ' + userToken } });return { status: response.status, body: await response.json() }; };
  await send({ type: 'host.capabilities', capabilities: { service: true, interactiveSession: true, streaming: false, virtualDisplayReady: false, monitors: [], diagnosticsSupported: true } });
  const raw = events => ({ protocol: 'host-diagnostics-v1', source: 'host-service', version: '0.2.146', collectedAt: new Date().toISOString(), interactiveSession: true, consoleSession: true, captureAvailable: true,
    display: { observed: true, observationAgeMs: 0, installed: true, deviceReady: true, owned: true, activeDisplay: true, configurationPending: false, recoveryRequired: false, rebootRequired: false, remoteSession: false, monitorCount: 0, width: 0, height: 0, reason: '' },
    native: { available: false }, events });
  assert.equal((await send({ type: 'host.diagnostics', report: raw([event(1), event(2, { action: 250, stage: 9, ok: false, token: 'DO_NOT_SEND' })]) })).accepted, true);
  store.db.prepare('UPDATE host_diagnostics SET received_at=?').run(Date.now() - 6000);
  assert.equal((await send({ type: 'host.diagnostics', report: raw([event(2, { action: 250, stage: 9, ok: false }), event(3)]) })).accepted, true);
  const view = await api();
  assert.equal(view.status, 200);
  assert.deepEqual(view.body.report.events.map(row => row.time), [at(2), at(3)], 'the latest report stays as it came');
  assert.equal(view.body.history.retentionDays, 7);assert.equal(view.body.history.limit, 500);
  assert.deepEqual(view.body.history.events.map(row => row.time), [at(3), at(2), at(1)]);
  assert.equal(JSON.stringify(view.body).includes('DO_NOT_SEND'), false);
  assert.equal((await api('alice')).status, 401, 'administrators only');
  // The VM page's timeline reads the history, each event with its own Host version.
  const ctx = { value: name => name === 'diagnostics:pve-149' ? view.body : name === 'hosts' ? { hosts: [{ desktopId: 'pve-149', diagnostics: { version: '0.2.146', events: view.body.report.events } }] } : null };
  const entries = hostEntries(desktopHostList(ctx, 'pve-149'));
  assert.equal(entries.length, 3);
  assert.ok(entries.every(entry => entry.version === '0.2.146' && entry.desktopId === 'pve-149'));
  assert.equal(entries.filter(entry => entry.failure).length, 1);
  // Without a history (an older broker, or none merged yet) it falls back to the latest report.
  const latest = [{ desktopId: 'pve-170', diagnostics: { events: [event(1)] } }];
  for (const diagnostics of [null, { history: { events: [] } }]) assert.deepEqual(desktopHostList({ value: name => name === 'hosts' ? { hosts: latest } : name === 'diagnostics:pve-170' ? diagnostics : null }, 'pve-170'), latest);
});

test('A client record is a failure by its ok; a code beside ok:true is information', () => {
  const entry = (values, code) => clientEntry({ time: at(0), event: 'drive', values, ...(code ? { code } : {}) });
  assert.equal(entry({ stage: 4, ok: true }, 'drive_session_closed').failure, false);
  assert.equal(entry({ stage: 4, ok: true }, 'drive_session_closed').code, 'drive_session_closed', 'the code is still shown');
  assert.equal(entry({ stage: 4, ok: false }, 'drive_transport_closed').failure, true);
  assert.equal(entry({ stage: 3, ok: false }).failure, true);
  assert.equal(entry({ stage: 1 }, 'unknown_failure').failure, true, 'a code without ok stays a failure (older clients)');
  assert.equal(entry({ stage: 1 }).failure, false);
});
