import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { parseHTML } from 'linkedom';

// Exercise the shipped application and DOM. Only imports and test exports are
// added in memory; fetch is synthetic and Node's clock advances the real 15s
// budget. No server, credentials, production exports or product writes.
const appUrl = new URL('../admin/app.js', import.meta.url);
const deferred = () => {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
};
const response = (body, status = 200) => ({ ok: status < 400, status, json: async () => body });
const inventory = owner => ({ owner, users: [], desktops: [], assignments: [], audit: [],
  proxmox: { configured: false }, onboarding: { complete: true, proxmoxRequired: false } });
const abortable = (promise, signal) => {
  if (!signal) return Promise.resolve(promise);
  return new Promise((resolve, reject) => {
    const finish = (ok, value) => {
      signal.removeEventListener('abort', aborted);
      if (ok) resolve(value); else reject(value);
    };
    const aborted = () => finish(false, signal.reason);
    if (signal.aborted) { aborted(); return; }
    signal.addEventListener('abort', aborted, { once: true });
    Promise.resolve(promise).then(value => finish(true, value), error => finish(false, error));
  });
};
let serial = 0;
async function app(t) {
  const window = parseHTML(await readFile(new URL('../admin/index.html', import.meta.url), 'utf8'));
  const keys = ['document', 'Node', 'fetch', 'FormData', 'location', 'sessionStorage', 'matchMedia', 'addEventListener'];
  const previous = Object.fromEntries(keys.map(key => [key, globalThis[key]]));
  globalThis.document = window.document; globalThis.Node = window.Node;
  globalThis.location = new URL('https://admin.fixture.invalid/admin/');
  globalThis.sessionStorage = { getItem: () => null, setItem() {} };
  globalThis.matchMedia = () => ({ matches: false, addEventListener() {} });
  globalThis.addEventListener = () => {};
  const formProto = Object.getPrototypeOf(document.querySelector('form'));
  formProto.reset = function () { for (const node of this.querySelectorAll('input,textarea')) node.value = ''; };
  Object.defineProperty(formProto, 'elements', { configurable: true, get() {
    return Object.fromEntries([...this.querySelectorAll('[name]')].map(node => [node.name, node]));
  } });
  globalThis.FormData = class {
    constructor(form) { this.rows = [...form.querySelectorAll('[name]')].map(node => [node.name, node.value]); }
    *[Symbol.iterator]() { yield* this.rows; }
  };
  const dialog = document.querySelector('#modal');
  dialog.showModal = () => { dialog.open = true; }; dialog.close = () => { dialog.open = false; };
  const user = { id: 'fixture-admin', username: 'fixture-admin', role: 'admin', mustChangeCredentials: false };
  const queues = new Map(), requests = [];
  let currentInventory = inventory('initial');
  const queue = (path, value, { ignoreAbort = false } = {}) => {
    const entries = queues.get(path) ?? []; entries.push({ value, ignoreAbort }); queues.set(path, entries);
  };
  globalThis.fetch = async (url, options = {}) => {
    const path = url.replace('/api/v1/admin', ''); requests.push({ path, ...options });
    const entry = queues.get(path)?.shift();
    const fallback = () => {
      if (url === '/healthz') return response({ service: 'multitap-broker', version: '1.0.15' });
      if (url === '/api/v1/downloads') return response({ releases: [] });
      if (path === '/session') return response({ user, csrfToken: 'fixture-only' });
      if (path === '/inventory') return response(currentInventory);
      if (path === '/server-info') return response({ service: 'multitap-broker', version: '1.0.15',
        deployment: { kind: 'unknown', configured: false }, update: { state: 'not_configured' } });
      if (path === '/proxmox/status') return response({ proxmox: currentInventory.proxmox });
      return response({ sessions: [], hosts: [], events: [], requests: [], pendingCount: 0 });
    };
    const pending = entry ? entry.value : fallback();
    // Some tests deliberately ignore AbortSignal to model a late response or
    // body from an adapter. The deadline and owner guards must still hold.
    if (entry?.ignoreAbort) return pending;
    const result = await abortable(pending, options.signal);
    return { ...result, json: () => abortable(result.json(), options.signal) };
  };
  const source = (await readFile(appUrl, 'utf8')).replace(/from '(\.[^']+)'/g,
    (_, relative) => `from '${new URL(relative, appUrl).href}'`);
  const instrumented = source + `\nexport const qa = {state,api,load,refresh,showLogin,enter,getLost:()=>serverLost};\n//# sourceURL=admin-read-deadline-${++serial}.js`;
  const { qa } = await import('data:text/javascript;base64,' + Buffer.from(instrumented).toString('base64'));
  t.after(() => { qa.showLogin(); for (const [key, value] of Object.entries(previous)) globalThis[key] = value; });
  const settle = async () => { for (let n = 0; n < 5; n++) await new Promise(resolve => setImmediate(resolve)); };
  await settle();
  return { qa, user, queue, requests, settle, node: selector => document.querySelector(selector),
    login: async () => { currentInventory = inventory('new-login'); await qa.enter({ user, csrfToken: 'fixture-only' }); await settle(); } };
}

for (const target of ['inventory', 'hosts']) test(`a stalled ${target} GET releases busy controls and allows one explicit retry`, async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'] });
  const h = await app(t), hold = deferred(); h.queue('/' + target, hold.promise);
  const count = () => h.requests.filter(row => row.path === '/' + target).length;
  const before = count(), run = () => target === 'inventory' ? h.qa.refresh() : h.qa.load('hosts', { force: true });
  const pending = run(); await h.settle();
  t.mock.timers.tick(14999); await h.settle();
  const busy = () => target === 'inventory' ? h.qa.state.loading : h.qa.state.resources.get('hosts').loading;
  assert.equal(busy(), true, 'the read gets the full 15-second budget');
  t.mock.timers.tick(1); await pending; await h.settle();
  assert.equal(busy(), false);
  assert.match(target === 'inventory' ? h.node('#notice').textContent : h.qa.state.resources.get('hosts').error, /새로고침/);
  assert.equal(count() - before, 1, 'no automatic retry');
  assert.equal(h.requests.filter(row => row.path === '/' + target).at(-1).signal.aborted, true);
  await run(); assert.equal(count() - before, 2, 'only the explicit retry adds a request');
  hold.resolve(response(target === 'inventory' ? inventory('late') : { hosts: [] })); await h.settle();
  assert.equal(h.requests.some(row => row.method && row.method !== 'GET'), false);
});

test('the deadline also aborts a stalled JSON body after headers arrive', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'] });
  const h = await app(t), body = deferred();
  h.queue('/body', { ok: true, status: 200, json: () => body.promise });
  const pending = h.qa.api('/body').catch(error => error); await h.settle();
  t.mock.timers.tick(15000);
  assert.equal((await pending).code, 'admin_read_timeout');
  assert.equal(h.requests.find(row => row.path === '/body').signal.aborted, true);
  body.resolve({ ok: true }); await h.settle();
});

for (const finish of ['unauthorized', 'setup-required']) test(`a late ${finish} response after timeout cannot change the current session`, async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'] });
  const h = await app(t), hold = deferred(), route = h.qa.state.route.page;
  h.queue('/late', hold.promise, { ignoreAbort: true });
  const pending = h.qa.api('/late').catch(error => error); await h.settle(); t.mock.timers.tick(15000);
  assert.equal((await pending).code, 'admin_read_timeout');
  hold.resolve(response({ error: { code: finish === 'unauthorized' ? 'unauthorized' : 'proxmox_setup_required' } }, finish === 'unauthorized' ? 401 : 409));
  await h.settle(); assert.equal(h.qa.state.user, h.user); assert.equal(h.qa.state.route.page, route);
  assert.equal(h.node('#console').hidden, false);
});

test('an old deadline cannot release a newer same-account refresh or replace its notice', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'] });
  const h = await app(t), old = deferred(), fresh = deferred(); h.queue('/inventory', old.promise);
  const first = h.qa.refresh(); await h.settle(); t.mock.timers.tick(14000);
  h.qa.showLogin(); await h.login(); h.queue('/inventory', fresh.promise);
  const second = h.qa.refresh(); await h.settle(); const notice = h.node('#notice').textContent;
  t.mock.timers.tick(1000); await first; await h.settle();
  assert.equal(h.qa.state.loading, true); assert.equal(h.node('#notice').textContent, notice);
  assert.equal(h.qa.getLost(), false);
  fresh.resolve(response(inventory('fresh'))); await second;
  old.resolve(response(inventory('old'))); await h.settle();
  assert.equal(h.qa.state.data.owner, 'fresh');
});

for (const method of ['POST', 'PUT', 'DELETE']) test(`${method} is never timed out, aborted or replayed by the GET deadline`, async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'] });
  const h = await app(t), hold = deferred(); h.queue('/write-control', hold.promise);
  let settled = false;
  const pending = h.qa.api('/write-control', method, { fixture: true }).then(value => { settled = true; return value; });
  await h.settle(); t.mock.timers.tick(60000); await h.settle();
  assert.equal(settled, false);
  const calls = h.requests.filter(row => row.path === '/write-control');
  assert.equal(calls.length, 1); assert.equal(calls[0].signal, undefined);
  assert.equal(calls[0].headers['X-CSRF-Token'], 'fixture-only');
  hold.resolve(response({ ok: true })); assert.deepEqual(await pending, { ok: true });
});

test('a successful GET clears its timer instead of aborting its signal later', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'] });
  const h = await app(t); h.queue('/complete', response({ ok: true }));
  assert.deepEqual(await h.qa.api('/complete'), { ok: true });
  const signal = h.requests.find(row => row.path === '/complete').signal;
  t.mock.timers.tick(15000); await h.settle(); assert.equal(signal.aborted, false);
});
