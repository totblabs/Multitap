import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseHTML } from 'linkedom';
import { requestCards } from '../admin/js/hosts.js';

// Shipped approval actions with an inert DOM and memory-only API. No actual
// enrollment, Proxmox lookup, Host or Windows operation is performed.
// The cards are rendered as 데스크톱 > 승인 대기 renders them: one card per request,
// with no Host kind chosen first. VM approval, ‘직접 입력’ and ‘로컬 PC로 등록’ are on that card.
const uuid = '12345678-1234-1234-1234-123456789abc';
const candidate = { desktopId: 'pve-245', vmid: 245, node: 'pve-node', name: 'KNOWN-VM', hasExistingHost: false };
function fixture(t, { candidates = [], codeDisplay = 'shown', vmLookup } = {}) {
  const old = { document: globalThis.document, Node: globalThis.Node };
  const window = parseHTML('<!doctype html><html><body></body></html>');
  globalThis.document = window.document; globalThis.Node = window.Node;
  t.after(() => Object.assign(globalThis, old));
  const request = { requestId: '00000000-0000-4000-8000-000000000001', machineUuid: uuid, hostname: 'HOST-PC', version: '1.0.12', status: 'pending', expiresAt: '2099-01-01T00:00:00Z', verificationCode: 'ABCD-EFGH', codeDisplay, candidates, duplicateRequestIds: [], ...(vmLookup ? { vmLookup } : {}), localPcRegistration: { allowed: candidates.length === 0 } };
  const value = { requests: [request] }, entry = { value }, writes = [], forms = [], notices = [], loads = [];
  const ctx = {
    state: { user: { id: 'admin-fixture' }, csrf: 'csrf-fixture', data: { desktops: [] } },
    resource: () => entry, value: () => value,
    api: async (path, method, body) => { writes.push({ path, method, body }); return { ok: true }; },
    modal: (title, fields, action, options) => { forms.push({ title, fields, action, options }); return forms.length; },
    notice: (...args) => notices.push(args), load: async (...args) => loads.push(args),
    pageAction: () => { const actor = ctx.state.user, csrf = ctx.state.csrf; const current = () => ctx.state.user === actor && ctx.state.csrf === csrf; return { current, check() { if (!current()) throw new Error('session changed'); } }; },
  };
  const render = () => { const root = requestCards(ctx); assert.ok(root.querySelectorAll('article.request-card').length <= 1, 'one card per request'); return root; };
  const button = (root, text) => [...root.querySelectorAll('button')].find(n => n.textContent === text);
  const openVm = () => { const control = button(render(), '확인하고 승인'); assert.ok(control); assert.equal(control.disabled, false); control.click(); assert.ok(forms[0]); return forms[0]; };
  // A request the server could not place: node and VM ID are typed behind the small ‘직접 입력’ link.
  const openManual = () => { const root = render(), control = button(root, '직접 입력'); assert.ok(control); assert.equal(control.disabled, false); assert.equal(!!button(root, '확인하고 승인'), false); control.click(); assert.ok(forms[0]); return forms[0]; };
  const openPc = () => { const control = button(render(), '로컬 PC로 등록'); assert.ok(control); assert.equal(control.disabled, false); control.click(); return forms[0]; };
  return { ctx, request, entry, writes, forms, notices, loads, render, button, openVm, openManual, openPc };
}
const explicit = extra => ({ node: 'pve-node', vmid: '245', acknowledgeCode: 'yes', replaceExisting: 'no', ...extra });
// The one checkbox ‘코드가 같아요’: FormData carries 'yes' only when it is ticked.
const codeBox = form => { const field = form.fields.find(x => x.name === 'acknowledgeCode'); assert.ok(field, 'the code tick is in the dialog'); assert.equal(field.checkbox, true); assert.equal(field.label, '코드가 같아요'); return field; };
const ticked = extra => ({ acknowledgeCode: 'yes', ...extra });

test('unknown Host opens blank explicit node/VM fields and sends only the confirmed exact mapping', async t => {
  const f = fixture(t), form = f.openManual();
  assert.equal(form.fields.find(x => x.name === 'node').value, '');
  assert.equal(form.fields.find(x => x.name === 'vmid').value, '');
  codeBox(form);
  assert.equal(form.fields.some(x => x.name === 'desktopId'), false); assert.deepEqual(f.writes, []);
  await form.action(explicit());
  assert.deepEqual(f.writes, [{ path: '/hosts/requests/' + f.request.requestId + '/approve', method: 'POST', body: { node: 'pve-node', vmid: 245, acknowledgeCode: true, replaceExisting: false } }]);
});
for (const input of [{ node: '' }, { node: '..' }, { node: 'wrong/node' }, { vmid: '99' }, { vmid: '245.1' }, { vmid: '1000000000' }, { acknowledgeCode: 'no' }]) test('explicit Host mapping rejects invalid or unconfirmed ' + JSON.stringify(input), async t => {
  const f = fixture(t), form = f.openManual();
  await assert.rejects(form.action(explicit(input))); assert.deepEqual(f.writes, []);
});
test('ambiguous known candidates require explicit mapping without choosing the first VM', t => {
  const f = fixture(t, { candidates: [candidate, { ...candidate, desktopId: 'pve-247', vmid: 247 }] });
  assert.match(f.render().textContent, /식별 정보가 같은 VM이 여러 대예요\./);
  const form = f.openManual();
  assert.equal(form.fields.some(x => x.name === 'desktopId'), false);
  assert.equal(form.fields.find(x => x.name === 'node').value, '');
});
test('single known candidate keeps the existing exact desktop contract without a cluster query', async t => {
  const f = fixture(t, { candidates: [candidate] }), form = f.openVm();
  codeBox(form);
  // Not ticked: nothing is sent.
  await assert.rejects(form.action({ desktopId: candidate.desktopId, replaceExisting: 'no' }), /코드가 같아요/); assert.deepEqual(f.writes, []);
  await form.action(ticked({ desktopId: candidate.desktopId, replaceExisting: 'no' }));
  assert.deepEqual(f.writes, [{ path: '/hosts/requests/' + f.request.requestId + '/approve', method: 'POST', body: { desktopId: candidate.desktopId, acknowledgeCode: true, replaceExisting: false } }]);
});
for (const change of ['request-id', 'machine-uuid', 'verification-code', 'administrator']) test('known approval never rebinds after ' + change + ' changes', async t => {
  const f = fixture(t, { candidates: [candidate] }), form = f.openVm();
  if (change === 'request-id') f.request.requestId = '00000000-0000-4000-8000-000000000002';
  if (change === 'machine-uuid') f.request.machineUuid = '87654321-4321-4321-4321-cba987654321';
  if (change === 'verification-code') f.request.verificationCode = 'DIFFERENT';
  if (change === 'administrator') f.ctx.state.csrf = 'csrf-new';
  await assert.rejects(form.action(ticked({ desktopId: candidate.desktopId }))); assert.deepEqual(f.writes, []);
});
test('unknown Host without displayed code stays disabled', t => {
  const f = fixture(t, { codeDisplay: 'unavailable' });
  const root = f.render(); assert.equal(!!f.button(root, '확인하고 승인'), false);
  assert.equal(f.button(root, '직접 입력').disabled, true); f.button(root, '직접 입력').click(); assert.deepEqual(f.forms, []);
  // The same card's local PC registration needs the displayed code as well.
  assert.equal(f.button(root, '로컬 PC로 등록').disabled, true); f.button(root, '로컬 PC로 등록').click(); assert.deepEqual(f.forms, []); assert.deepEqual(f.writes, []);
});
test('request-list failure never offers an approval from cached requests', t => {
  const f = fixture(t, { candidates: [candidate] }); f.entry.error = 'fixture request unavailable';
  const root = f.render(); assert.equal(!!f.button(root, '확인하고 승인'), false);
  assert.match(root.textContent, /확인하지 못했어요/);
});
test('unlinked Host registration requires separate explicit consent and sends it to the broker', async t => {
  const f = fixture(t), form = f.openPc();
  assert.equal(form.fields.find(x => x.name === 'acknowledgeUnlinked')?.value, 'no');
  assert.match(form.options.description, /Proxmox 연결 없이/);
  await assert.rejects(form.action({ name: 'WORK', acknowledgeCode: 'yes', acknowledgeUnlinked: 'no' })); assert.deepEqual(f.writes, []);
  await form.action({ name: 'WORK', acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes' });
  assert.deepEqual(f.writes[0].body, { name: 'WORK', acknowledgeCode: true, acknowledgeUnlinked: true });
});

test('deleted unlinked Host uses captured previous registration even when absent from the main inventory', async t => {
  const f = fixture(t), previous = 'pc-00000000-0000-4000-8000-000000000009';
  f.request.localPcRegistration = { allowed: true, existingDesktopId: previous, existingName: 'PREVIOUS' };
  const form = f.openPc(); assert.equal(f.ctx.state.data.desktops.length, 0);
  assert.ok(form.fields.find(x => x.name === 'desktopId').options.some(([id]) => id === previous));
  await assert.rejects(form.action({ name: 'WORK', acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes', desktopId: previous, replaceExisting: 'no' }));
  assert.deepEqual(f.writes, []);
  await form.action({ name: 'WORK', acknowledgeCode: 'yes', acknowledgeUnlinked: 'yes', desktopId: previous, replaceExisting: 'yes' });
  assert.deepEqual(f.writes[0].body, { name: 'WORK', acknowledgeCode: true, acknowledgeUnlinked: true, desktopId: previous, replaceExisting: true });
});
test('pending and committed approval callbacks never submit twice', async t => {
  const f = fixture(t); let finish;
  f.ctx.api = (path, method, body) => { f.writes.push({ path, method, body }); return new Promise(resolve => { finish = resolve; }); };
  const form = f.openManual(), first = form.action(explicit());
  await form.action(explicit()); assert.equal(f.writes.length, 1);
  finish({ ok: true }); await first;
  await form.action(explicit()); assert.equal(f.writes.length, 1);
});
test('approval response from a previous administrator cannot notify or reload the new session', async t => {
  const f = fixture(t, { candidates: [candidate] }); let finish;
  f.ctx.api = (path, method, body) => { f.writes.push({ path, method, body }); return new Promise(resolve => { finish = resolve; }); };
  const form = f.openVm(), first = form.action(ticked({ desktopId: candidate.desktopId }));
  f.ctx.state.csrf = 'csrf-new'; finish({ ok: true });
  await assert.rejects(first); assert.deepEqual(f.notices, []); assert.deepEqual(f.loads, []);
  assert.equal(f.writes.length, 1);
});
test('changed known VM mapping or added rival cannot inherit an old approval', async t => {
  const f = fixture(t, { candidates: [{ ...candidate }] }), form = f.openVm();
  f.request.candidates[0].vmid = 247;
  await assert.rejects(form.action(ticked({ desktopId: candidate.desktopId }))); assert.deepEqual(f.writes, []);
  f.request.candidates[0].vmid = candidate.vmid; f.request.duplicateRequestIds.push('new-rival');
  await assert.rejects(form.action(ticked({ desktopId: candidate.desktopId }))); assert.deepEqual(f.writes, []);
});
test('malformed request inventory renders a retryable error without an approval', t => {
  const f = fixture(t); f.entry.value.requests = null;
  const root = f.render(); assert.match(root.textContent, /확인하지 못했어요/);
  assert.equal(!!f.button(root, '확인하고 승인'), false);
});

// The server finds a new VM by the request's SMBIOS
// UUID (vmLookup), so the administrator is not asked for node and VM ID.
const found = { node: 'pve-node', vmid: 245, name: 'NEW-VM', desktopId: null, hasExistingHost: false };
const matched = extra => ({ state: 'matched', vm: { ...found, ...extra } });
test('a VM the server found in Proxmox is approved without typing node or VM ID, with the one code tick', async t => {
  const f = fixture(t, { vmLookup: matched() }), root = f.render();
  assert.match(root.textContent, /Proxmox에서 찾은 VM: VM 245 · NEW-VM \(pve-node\)/);
  assert.equal(!!f.button(root, '직접 입력'), false);
  const form = f.openVm();
  for (const name of ['node', 'vmid', 'desktopId', 'replaceExisting']) assert.equal(form.fields.some(x => x.name === name), false, name);
  codeBox(form);
  const target = form.fields.find(x => x.label === '연결할 Proxmox VM');
  assert.equal(target.value, 'VM 245 · NEW-VM (pve-node)'); assert.equal(target.readonly, true);
  assert.ok(form.fields.some(x => x.label === '확인 코드'), 'the code stays on screen to compare');
  await assert.rejects(form.action({}), /코드가 같아요/); assert.deepEqual(f.writes, []);
  await form.action(ticked());
  assert.deepEqual(f.writes, [{ path: '/hosts/requests/' + f.request.requestId + '/approve', method: 'POST', body: { node: 'pve-node', vmid: 245, serverMatch: true, acknowledgeCode: true, replaceExisting: false } }]);
});
test('a found VM that already has a Host still needs the explicit replacement choice', async t => {
  const f = fixture(t, { vmLookup: matched({ desktopId: 'pve-245', hasExistingHost: true }) }), root = f.render();
  assert.match(root.textContent, /VM 245 · NEW-VM \(pve-node\) \(기존 연결 있음\)/);
  const form = f.openVm(); assert.equal(form.fields.find(x => x.name === 'replaceExisting').value, 'no');
  await assert.rejects(form.action({ replaceExisting: 'yes' }), /코드가 같아요/); assert.deepEqual(f.writes, []);
  await form.action(ticked({ replaceExisting: 'yes' }));
  assert.deepEqual(f.writes[0].body, { node: 'pve-node', vmid: 245, serverMatch: true, acknowledgeCode: true, replaceExisting: true });
});
test('a Host that shows no code is not approvable on any path: found VM, a VM the table knows, or typed', t => {
  for (const options of [{ vmLookup: matched() }, { candidates: [candidate] }]) {
    const f = fixture(t, { ...options, codeDisplay: 'unavailable' }), control = f.button(f.render(), '확인하고 승인');
    assert.equal(control.disabled, true); control.click(); assert.deepEqual(f.forms, []); assert.deepEqual(f.writes, []);
  }
});
for (const change of ['node', 'vmid', 'existing-host', 'lost', 'now-known']) test('found VM approval never rebinds after the lookup result changes: ' + change, async t => {
  const f = fixture(t, { vmLookup: matched() }), form = f.openVm();
  if (change === 'node') f.request.vmLookup = matched({ node: 'other-node' });
  if (change === 'vmid') f.request.vmLookup = matched({ vmid: 246 });
  if (change === 'existing-host') f.request.vmLookup = matched({ hasExistingHost: true });
  if (change === 'lost') f.request.vmLookup = { state: 'not_found' };
  if (change === 'now-known') f.request.candidates = [candidate];
  await assert.rejects(form.action(ticked()), /Proxmox에서 찾은 VM이 바뀌었어요/); assert.deepEqual(f.writes, []);
});
for (const [vmLookup, reason] of [[{ state: 'not_found' }, 'Proxmox에서 맞는 VM을 찾지 못했어요.'], [{ state: 'unavailable', errorCode: 'proxmox_unreachable' }, 'Proxmox에 연결하지 못했어요.'],
  [{ state: 'unavailable', errorCode: 'proxmox_lookup_incomplete' }, 'Proxmox에서 맞는 VM을 찾지 못했어요.'], [{ state: 'ambiguous', count: 2 }, '식별 정보가 같은 VM이 여러 대예요.'], [undefined, 'Proxmox에서 맞는 VM을 찾지 못했어요.']]) test('an unplaced request shows one reason line and the manual link: ' + (vmLookup?.errorCode ?? vmLookup?.state ?? 'no lookup field'), t => {
  const f = fixture(t, { vmLookup }), root = f.render(), line = root.querySelector('.request-match');
  assert.equal(line.querySelector('span').textContent, reason);
  assert.equal(line.querySelector('button').textContent, '직접 입력');
  assert.equal(!!f.button(root, '확인하고 승인'), false);
  // Not placed on a VM: the same card also offers registration as a local PC, with one line saying when.
  assert.deepEqual([...root.querySelectorAll('article.request-card .actions button')].map(n => n.textContent), ['로컬 PC로 등록', '거절']);
  assert.equal(root.querySelector('.request-pc-hint').textContent, 'Proxmox 밖의 PC라면 로컬 PC로 등록하세요.');
  const form = f.openManual(); assert.equal(form.fields.find(x => x.name === 'node').value, '');
});
test('while the server is still looking no VM approval or manual entry is offered yet', t => {
  const f = fixture(t, { vmLookup: { state: 'searching' } }), root = f.render();
  assert.equal(root.querySelector('.request-match span').textContent, 'Proxmox에서 VM을 찾고 있어요.');
  assert.equal(!!f.button(root, '직접 입력'), false); assert.equal(!!f.button(root, '확인하고 승인'), false);
  assert.ok(f.button(root, '거절'));
});
test('a request the server placed on a Proxmox VM is not offered as a local PC', t => {
  const f = fixture(t, { vmLookup: matched() }), root = f.render();
  assert.equal(f.request.localPcRegistration.allowed, true, 'even when the request metadata would permit it');
  assert.ok(f.button(root, '확인하고 승인'));
  assert.equal(!!f.button(root, '로컬 PC로 등록'), false); assert.equal(root.querySelector('.request-pc-hint'), null);
  // The same for a VM the table already knows.
  const known = fixture(t, { candidates: [candidate] }); known.request.localPcRegistration = { allowed: true };
  assert.ok(known.button(known.render(), '확인하고 승인')); assert.equal(!!known.button(known.render(), '로컬 PC로 등록'), false);
});
test('the dashboard row names the found VM and approves it through the same dialog', async t => {
  const { requestVm, requestApproveButton } = await import('../admin/js/hosts.js');
  const f = fixture(t, { vmLookup: matched() });
  assert.deepEqual(requestVm(f.request), found);
  assert.equal(requestVm({ ...f.request, candidates: [candidate] }), candidate);
  assert.equal(requestVm({ ...f.request, candidates: [candidate, candidate] }), null);
  assert.equal(requestVm({ ...f.request, vmLookup: { state: 'not_found' } }), null);
  const control = requestApproveButton(f.ctx, f.request, { label: '확인하고 승인' });
  assert.equal(control.textContent, '확인하고 승인'); assert.equal(control.disabled, false); control.click();
  await f.forms[0].action(ticked());
  assert.deepEqual(f.writes[0].body, { node: 'pve-node', vmid: 245, serverMatch: true, acknowledgeCode: true, replaceExisting: false });
});
