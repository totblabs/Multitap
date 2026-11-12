// The DOM harness and fixtures shared by the admin-ui*.test.mjs files.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parseHTML } from 'linkedom';

// DOM behaviour only: this is not a browser layout/screenshot or real-server
// authentication test. `monitor` holds the fixture answers of the read-only
// monitoring endpoints and may be changed by a test between refreshes.
// A tab's sessionStorage; `storage: null` is a browser that refuses storage.
export const memoryStorage = () => { const items = new Map(); return { items, getItem: key => items.get(key) ?? null, setItem: (key, value) => { items.set(key, String(value)); } }; };
export async function loadAdmin(t, suffix, user, inventory, { origin = 'https://cloud.fixture.example', respond, deploymentKind = 'unknown', hash = '', monitor = {}, storage = memoryStorage(), narrow = false } = {}) {
  const window = parseHTML(await readFile(new URL('../admin/index.html', import.meta.url), 'utf8'));
  const previous = Object.fromEntries(['document', 'Node', 'fetch', 'FormData', 'location', 'sessionStorage', 'matchMedia', 'addEventListener'].map(key => [key, globalThis[key]]));
  t.after(() => { for (const [key, value] of Object.entries(previous)) globalThis[key] = value; });
  globalThis.document = window.document; globalThis.Node = window.Node; globalThis.sessionStorage = storage ?? undefined;
  globalThis.location = new URL(origin + '/admin/' + hash);
  globalThis.matchMedia = () => ({ matches: narrow, addEventListener() {} });
  const windowListeners = new Map();
  globalThis.addEventListener = (name, listener) => { const list = windowListeners.get(name) ?? []; list.push(listener); windowListeners.set(name, list); };
  const formProto = Object.getPrototypeOf(document.querySelector('form'));
  formProto.reset = function () { for (const node of this.querySelectorAll('input,textarea')) node.value = ''; };
  Object.defineProperty(formProto, 'elements', { configurable: true, get() { return Object.fromEntries([...this.querySelectorAll('[name]')].map(node => [node.name, node])); } });
  globalThis.FormData = class {
    // As a browser does: an unticked checkbox or radio sends nothing.
    constructor(form) { this.entries = [...form.querySelectorAll('input[name],select[name],textarea[name]')].filter(node => !['checkbox', 'radio'].includes(node.type) || node.checked).map(node => [node.name, node.value]); }
    *[Symbol.iterator]() { yield* this.entries; }
  };
  const dialog = document.querySelector('#modal');
  dialog.showModal = () => { dialog.open = true; }; dialog.close = () => { dialog.open = false; };
  // linkedom has no focus model: record the element the console focuses last.
  let focused = null; const focusProto = window.HTMLElement.prototype, ownFocus = Object.getOwnPropertyDescriptor(focusProto, 'focus');
  focusProto.focus = function () { focused = this; };
  t.after(() => { if (ownFocus) Object.defineProperty(focusProto, 'focus', ownFocus); else delete focusProto.focus; });
  const writes = [], requests = [];
  const ok = (value, status = 200) => ({ ok: status < 400, status, json: async () => value });
  globalThis.fetch = async (url, options = {}) => {
    requests.push(url);
    if (options.method && options.method !== 'GET') writes.push({ path: url, data: options.body ? JSON.parse(options.body) : null });
    if (respond) { const value = await respond(url, options); if (value) return value; }
    if (url === '/api/v1/downloads') return ok({ releases: monitor.releases ?? [] });
    const path = url.replace('/api/v1/admin', '');
    if (path === '/server-info') return ok({ service: 'multitap-broker', version: '0.8.1', deployment: { kind: deploymentKind, configured: deploymentKind !== 'unknown' }, update: { state: 'not_configured', feedConfigured: false, applySupported: false }, checkedAt: '2026-10-23T06:10:00.000Z' });
    if (path === '/proxmox' && options.method === 'PUT') {
      const input = JSON.parse(options.body);
      inventory.proxmox = { ...inventory.proxmox, configured: true, endpoint: input.endpoint, tokenId: input.tokenId, hasSecret: true, ca: input.ca || '', expiresAt: input.expiresAt || null, expirySource: input.expiresAt ? 'configured' : 'unknown' };
    }
    if (path === '/proxmox/sync') {
      inventory.proxmox = { ...inventory.proxmox, state: 'ok', checkedAt: '2026-10-22T06:00:00.000Z' };
      if (inventory.onboarding) inventory.onboarding = { stage: 'complete', credentialsRequired: false, proxmoxRequired: false, complete: true };
    }
    let batchResult;
    if (path === '/assignments/batch' && options.method === 'PUT') {
      // Fixture of the server contract: an omitted per-VM policy keeps the saved value.
      const input = JSON.parse(options.body), ids = input.assignments ? input.assignments.map(a => a.desktopId) : input.desktopIds;
      const prior = inventory.assignments.filter(a => a.username === input.username), old = new Set(prior.map(a => a.desktop_id));
      batchResult = { ok: true, added: ids.filter(id => !old.has(id)).length, removed: [...old].filter(id => !ids.includes(id)).length, count: ids.length, preservedUnavailable: 0 };
      inventory.assignments = inventory.assignments.filter(a => a.username !== input.username).concat(ids.map(id => {
        const saved = prior.find(a => a.desktop_id === id), entry = input.assignments?.find(a => a.desktopId === id);
        return { username: input.username, desktop_id: id, clipboardPolicy: entry?.clipboardPolicy ?? saved?.clipboardPolicy, devicePolicy: entry?.devicePolicy ?? saved?.devicePolicy, soundCardAdd: entry?.soundCardAdd ?? saved?.soundCardAdd ?? false };
      }));
    }
    const onboarding = inventory.onboarding ? { onboarding: inventory.onboarding } : {};
    // 모니터링 (server/src/usage.mjs): `monitor.usage` may answer a path; otherwise an install without history.
    if (path.startsWith('/usage/') && (!options.method || options.method === 'GET')) {
      const given = monitor.usage?.(path); if (given) return ok(given);
      const now = new Date(), pad = n => String(n).padStart(2, '0'), today = `${now.getFullYear()}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}`, zone = { protocol: 'usage-v1', timeZone: 'Asia/Seoul', utcOffsetMinutes: 540 };
      if (path === '/usage/live') return ok({ ...zone, checkedAt: now.toISOString(), today: { date: today, logins: 0, connections: 0 }, totals: { logins: 0, connections: 0 }, recordingSince: null, retentionDays: 730, people: 0, logins: [], connections: [] });
      const [, kind, from, to] = path.split('/').slice(1);
      if (kind === 'summary') return ok({ ...zone, from, to, days: [from], truncated: false, recordingSince: null, retentionDays: 730, totals: { logins: 0, connections: 0, connectedMs: 0, firstAt: null, lastAt: null, daily: [0], dailyConnections: [0], activeMs: null }, users: [], desktops: [] });
      return ok({ ...zone, from, to, page: 1, pageSize: 200, more: false, counts: { logins: 0, connections: 0 }, accounts: [], events: [] });
    }
    // 공지 (server/src/notices.mjs): `monitor.notices` is the answer of GET /notices; otherwise nobody is signed in and nothing was sent.
    if (path === '/notices' && (!options.method || options.method === 'GET')) return ok(monitor.notices ?? { protocol: 'notice-v1', limits: { title: 60, body: 500, bodyLines: 12, generalHours: 24, maintenanceDays: 30, sendsPerMinute: 10, retentionDays: 180 }, audience: { people: 0, apps: 0, logins: [], connections: [] }, notices: [] });
    // 일괄 Host 업데이트, the server contract (admin-http.mjs): one row for EVERY registered desktop, started only where the Host can take the update.
    if (path === '/host-updates/all' && options.method === 'POST') {
      const rows = monitor.hosts ?? inventory.fixtureHosts ?? [];
      return ok({ version: JSON.parse(options.body).version, results: rows.map(row => { const view = inventory.hostUpdates?.desktops?.[row.desktopId];
        return view?.available ? { desktopId: row.desktopId, started: true, status: 'dispatched' } : { desktopId: row.desktopId, started: false, reason: !view?.connected ? 'host_offline' : !view.newer ? 'host_update_current' : 'host_update_in_progress' }; }) }, 202);
    }
    const vmPath = /^\/desktops\/([^/]+)\/(diagnostics|client-diagnostics)$/.exec(path);
    if (vmPath && (!options.method || options.method === 'GET')) return ok(vmPath[2] === 'diagnostics' ? monitor.diagnostics?.[vmPath[1]] ?? { supported: false, connected: false, receivedAt: null, stale: true, report: null } : { events: (monitor.clientEvents ?? []).filter(e => e.desktopId === vmPath[1]) });
    const payload = path === '/session' ? { user, csrfToken: 'fixture-csrf', ...onboarding } : path === '/inventory' ? inventory : path === '/proxmox/status' ? { proxmox: inventory.proxmox, ...onboarding }
      : path === '/proxmox/sync' ? { state: 'ok', proxmox: inventory.proxmox, count: inventory.desktops.length, ...onboarding }
      : path === '/sessions' ? { sessions: monitor.sessions ?? [], checkedAt: new Date().toISOString() } : path === '/hosts' ? { hosts: monitor.hosts ?? inventory.fixtureHosts ?? [], checkedAt: new Date().toISOString() }
      : path === '/stream-stats' ? { protocol: 'stream-stats-v1', retentionDays: 7, limit: 480, truncated: false, sessions: monitor.streamStats ?? [] }
      : path === '/client-diagnostics' ? { protocol: 'client-diagnostics-v1', retentionDays: 7, events: monitor.clientEvents ?? [] }
      : path === '/hosts/requests' ? { requests: monitor.requests ?? [], pendingCount: (monitor.requests ?? []).length }
      : path === '/host-updates' ? inventory.hostUpdates ?? { latest: null, desktops: {} } : batchResult || { ok: true };
    return ok(payload);
  };
  await import('../admin/app.js?' + suffix);
  const content = document.querySelector('#content');
  const find = (scope, selector, text) => [...scope.querySelectorAll(selector)].find(node => node.textContent === text);
  const click = (scope, text) => {
    const node = find(scope, 'button', text);
    assert.ok(node, `Missing button: ${text}`); node.dispatchEvent(new window.Event('click')); return node;
  };
  // Follows an in-console link (anchor) by its exact text.
  const follow = (scope, text) => {
    const node = find(scope, 'a', text) ?? [...scope.querySelectorAll('a.account-choice')].find(node => node.getAttribute('aria-label') === text);
    assert.ok(node, `Missing link: ${text}`); node.dispatchEvent(new window.Event('click')); return node;
  };
  // Every page is a sidebar entry (overview, desktops, users, log, monitoring,
  // updates, downloads and the 설정 group: proxmox, server-access, backup,
  // server-info, licenses); during the first setup the two-page bar
  // offers proxmox and server-access as well.
  // 'log/<tab>' opens 기록 and then that tab (quality, admin);
  // 'monitoring/<tab>' opens 모니터링 and then users, desktops or history.
  const navigate = page => {
    const [place, tab] = page.split('/');
    let node = document.querySelector('#sidebar [data-page=' + place + ']');
    if (!node) node = document.querySelector('.section-navigation a[href="#/' + place + '"]');
    assert.ok(node, 'Missing navigation: ' + page); node.dispatchEvent(new window.Event('click'));
    if (tab) { const button = content.querySelector((place === 'monitoring' ? '#usage-tab-' : '#log-tab-') + tab); assert.ok(button, `Missing ${place} tab: ${tab}`); button.dispatchEvent(new window.Event('click')); }
  };
  const pickVm = id => {
    const node = content.querySelector('.vm-list-item[data-focus="vm-pick-' + id + '"]');
    assert.ok(node, `Missing VM choice: ${id}`); node.dispatchEvent(new window.Event('click')); return node;
  };
  const settle = async () => { for (let i = 0; i < 40; i++) await new Promise(resolve => setImmediate(resolve)); };
  const change = (node, value) => { if (typeof value === 'boolean') node.checked = value; else Object.defineProperty(node, 'value', { configurable: true, value }); node.dispatchEvent(new window.Event('change')); };
  const crumb = () => document.querySelector('#breadcrumb').textContent;
  await settle();
  const emitWindow = name => { const event = new window.Event(name, { cancelable: true }); for (const listener of windowListeners.get(name) ?? []) listener(event); return event; };
  return { window, writes, requests, dialog, content, click, follow, find, navigate, pickVm, settle, change, crumb, monitor, emitWindow, focused: () => focused };
}
export const iso = offset => new Date(Date.now() + offset).toISOString();
export const workspaceFixture = () => {
  const user = { id: 'workspace-owner', username: '관리자', role: 'admin', mustChangeCredentials: false };
  const vm = (vmid, name, power, os) => ({ id: 'pve-' + vmid, pve_vmid: vmid, name, pve_node: 'pve-fixture', pve_available: 1,
    pve_power_status: power, pve_os_family: os, pve_os_source: 'proxmox-config', pve_os_type: os === 'windows' ? 'win11' : 'l26',
    host: { connected: power === 'running', version: '0.2.86' }, connectionModes: { stream: power === 'running' } });
  return { user, inventory: { users: [{ ...user, disabled: 0, notes: '' }], assignments: [], audit: [], proxmox: { configured: true, state: 'ok' },
    fixtureHosts: [101,102,170].map(vmid => ({desktopId:'pve-'+vmid,connected:vmid!==102,version:'0.2.86',enrolledAt:iso(-86400000)})),
    desktops: [vm(170, '업무 B', 'running', 'windows'), vm(102, 'Linux 보관', 'stopped', 'linux'), vm(101, '업무 A', 'running', 'windows')] } };
};

export const registerFixtureDesktops = inventory => {
  inventory.fixtureHosts = inventory.desktops.filter(d => d.kind !== 'pc' || d.localPcApproved === true).map((d, index) => ({ desktopId: d.id, hostId: `00000000-0000-4000-8000-${String(index + 1).padStart(12, '0')}`, connected: d.host?.connected === true, version: d.host?.version ?? '1.0.12', enrolledAt: iso(-86400000) }));
};
export const registeredWorkspaceFixture = () => {
  const {user,inventory}=workspaceFixture();delete inventory.fixtureHosts;
  inventory.desktops=Array.from({length:30},(_,i)=>({id:'pve-'+(100+i),kind:'vm',pve_vmid:100+i,pve_node:'fixture',pve_available:1,name:(i<5?'Infrastructure ':'Windows ')+(100+i),pve_os_family:i<5?'linux':'windows',pve_power_status:i===5?'stopped':'running',host:{connected:i>=7&&i<=10}}));
  inventory.desktops[0].host.connected=true; // Runtime availability is not proof of registration.
  inventory.desktops.push({id:'pc-fixture',kind:'pc',name:'Approved PC',localPcApproved:true,host:{connected:false}});
  inventory.assignments=[{username:user.username,desktop_id:'pve-100'}];
  const hosts=[105,106,107,108,109,110].map(vmid=>({desktopId:'pve-'+vmid,connected:vmid>=107,hostname:'fixture-'+vmid,version:'1.0.0',enrolledAt:iso(-86400000),lastSeenAt:iso(-3600000),diagnostics:null}));
  hosts.push({desktopId:'pc-fixture',connected:false,version:'1.0.0',enrolledAt:iso(-86400000),diagnostics:null});
  return{user,inventory,monitor:{hosts}};
};
export const clipDenyFixture = { textToRemote: false, textToLocal: false, filesToRemote: false, filesToLocal: false };
export const deviceKeysFixture = ['drives', 'printers', 'usbDevices', 'smartCards', 'serialPorts'];
export const deviceDenyFixture = Object.fromEntries(deviceKeysFixture.map(key => [key, false]));
export function permissionFixture() {
  const user = { id: 'fixture-primary', username: '메인관리자', role: 'admin', isPrimaryAdmin: true, mustChangeCredentials: false };
  const member = { id: 'fixture-member', username: '사용자', notes: '보존할 비고', role: 'user', disabled: 0 };
  const desktops = [101, 102, 103].map(vmid => ({ id: 'pve-' + vmid, pve_vmid: vmid, name: '업무 ' + vmid, os: '미확인', pve_node: 'pve', pve_available: 1, pve_power_status: 'running' }));
  // pve-101 already carries a saved text→VM grant; pve-102 is assigned and denied.
  const inventory = { users: [{ id: user.id, username: user.username, role: 'admin', disabled: 0, is_primary_admin: 1, notes: '' }, member], desktops, audit: [], proxmox: { configured: false },
    assignments: [{ username: member.username, desktop_id: 'pve-101', clipboardPolicy: { ...clipDenyFixture, textToRemote: true }, devicePolicy: deviceDenyFixture }, { username: member.username, desktop_id: 'pve-102', clipboardPolicy: clipDenyFixture, devicePolicy: deviceDenyFixture }] };
  registerFixtureDesktops(inventory);
  return { user, member, inventory };
}

export const ownerFixture = () => { const user = { id: 'fixture-owner', username: 'owner', role: 'admin', mustChangeCredentials: false }; return { user, inventory: { users: [{ ...user, disabled: 0, notes: '' }], desktops: [], assignments: [], audit: [], proxmox: { configured: true } } }; };
export const recheck = async f => { document.querySelector('#refresh').dispatchEvent(new f.window.Event('click')); await f.settle(); };

// Server information whose running version a test changes, as an update does.
export function versionServer(version) {
  const server = { version };
  server.respond = async url => url.endsWith('/server-info') ? { ok: true, status: 200, json: async () => ({ service: 'multitap-broker', version: server.version, deployment: { kind: 'systemd', configured: true }, update: { state: 'current', feedConfigured: true, applySupported: true }, checkedAt: new Date().toISOString() }) } : null;
  return server;
}
